#!/usr/bin/env python3
"""
Bake the cheat engine into a copy of a ROM file, so it can be tested in ares in seconds.

    tools/rompatch.py IN.z64 -o OUT.z64 --code 8011B99C 0001
    tools/rompatch.py IN.z64 --list-gaps          # where the engine could live, and why
    tools/rompatch.py OUT.z64 --check             # does a patched image still add up?

## Why this exists

Eight hardware rounds have now gone into making a cheat run on the M64, and the loop is the
problem: build, deploy, hand over the SD card, wait, read a photograph of a black screen. Four of
those eight were void because the probe itself was wrong, and a black screen cannot tell you
which half broke.

Every one of those attempts also shared an assumption -- that the engine gets into the machine at
*runtime*, written into cartridge SDRAM or into RDRAM microseconds before the handoff, and has to
survive a boot that this console demonstrably does not carry state across. AUDIT 2q, 2r, 2s, 2u
and 2x are five different answers to "where does the code live", and all five are answers to a
question that only exists because the code arrives late.

It does not have to. The engine is thirty-odd instructions and IPL3 copies a megabyte of the ROM
into RDRAM before the game's first instruction runs. Put the engine inside that megabyte and the
console installs it for us, from the cartridge, through the same DMA that loads the game -- no
survival, no cart write, no timing. What is left is a file, which a PC can build and an emulator
can boot, and the round trip is a second instead of a day.

## What it does to the image

Three edits, all inside ROM [0x1000, 0x101000) -- the window IPL3 loads and every retail IPL3
checksums:

1.  The engine goes into a run of zero padding between two functions of the game's own boot code.
    IPL3 lands it in RDRAM at a fixed address, cached, with no PI fetch at run time.
2.  The two words at the top of `__osExceptionPreamble` are rewritten to compute the engine's
    address instead of `__osException`'s. `osInitialize` then copies our four words onto the
    exception vectors itself, which is the whole trick: the game installs the hook.
3.  CRC1/CRC2 in the header are recomputed over the changed window, because otherwise the console
    stops dead before the logo.

The engine ends by replaying the preamble's own first two words and `jr $k0`, so `__osException`
is entered with exactly the register state it had before -- `$k0` holding its own address, which
is the part the inline-stub attempt got wrong (AUDIT 2x).

## The gap is the only judgement call, and it is where the last attempt died

`rompatch.c`'s in-image mode took the *last* long-enough run of zeros in the window, on the
reasoning that padding collects at the tail. It does not. On Ocarina that rule picks ROM 0x04125c,
9,204 bytes of zeros sitting inside compressed asset data at RDRAM 0x8004065c, which the game
reuses within a second of booting.

The rule here is the opposite and is checked rather than assumed: take the *lowest* run that is
long enough, cap the length so a data void cannot qualify, and require real instructions either
side. On Ocarina that picks ROM 0x004174 -- 108 bytes between a function ending in `jr $ra` and
one beginning with `mtc0`, at RDRAM 0x80003574, inside resident boot code below the bss the entry
stub clears. --list-gaps prints the candidates with their neighbours so the choice can be looked
at rather than trusted.
"""

import argparse
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import romcrc
import preamblescan

WINDOW_OFF = 0x1000
WINDOW_LEN = 0x100000

# IPL3 does not always load to the header's entry point; two CICs shift it. Same two numbers as
# rom_info.c's fix_boot_address() and preamblescan.py's table.
CIC_SHIFT = {"6103": -0x100000, "6106": -0x200000}

REG_ZERO, REG_K0, REG_K1 = 0, 26, 27

# The gap policy, in numbers. GUARD words of zero are left untouched either side so a
# fencepost error in the scan cannot land on an instruction. MAX_GAP rejects the multi-kilobyte
# voids inside compressed data that the last attempt fell into: real inter-function padding is
# tens of bytes, never thousands.
GAP_GUARD_WORDS = 2
GAP_MAX_BYTES = 0x400


def _op(op, rs, rt, imm):
    return ((op & 0x3F) << 26) | ((rs & 0x1F) << 21) | ((rt & 0x1F) << 16) | (imm & 0xFFFF)


def i_lui(rt, imm):             return _op(0x0F, 0, rt, imm)
def i_ori(rt, rs, imm):         return _op(0x0D, rs, rt, imm)
def i_addiu(rt, rs, imm):       return _op(0x09, rs, rt, imm)
def i_sb(rt, off, base):        return _op(0x28, base, rt, off)
def i_sh(rt, off, base):        return _op(0x29, base, rt, off)
def i_sw(rt, off, base):        return _op(0x2B, base, rt, off)
def i_jr(rs):                   return ((rs & 0x1F) << 21) | 0x08
def i_j(target):                return 0x08000000 | ((target >> 2) & 0x03FFFFFF)
def i_nop():                    return 0


def split_addr(addr):
    """The lui/offset pair for a 32-bit address, with addiu's sign extension paid for.

    Store offsets are sign-extended, so an address whose low half has bit 15 set needs the high
    half biased up by one -- 0x8011B99C is `lui 0x8012` and offset 0xB99C, not `lui 0x8011`.
    Getting this backwards writes 64KB away from the intended address and nothing appears to
    happen, which is a failure mode worth naming.
    """
    hi = (addr >> 16) & 0xFFFF
    lo = addr & 0xFFFF
    if lo & 0x8000:
        hi = (hi + 1) & 0xFFFF
    return hi, lo


def identify(rom):
    crc = zlib.crc32(rom[0x40:0x1000]) & 0xFFFFFFFF
    cic = romcrc.IPL3_CRC.get(crc)
    if cic is None:
        raise SystemExit("unknown IPL3 (crc32 %08x): refusing to touch this image" % crc)
    header_entry = struct.unpack_from(">I", rom, 8)[0]
    return cic, header_entry, (header_entry + CIC_SHIFT.get(cic, 0)) & 0xFFFFFFFF


def zero_runs(rom):
    runs, i, end = [], WINDOW_OFF, WINDOW_OFF + WINDOW_LEN
    while i < end:
        if struct.unpack_from(">I", rom, i)[0] == 0:
            j = i
            while j < end and struct.unpack_from(">I", rom, j)[0] == 0:
                j += 4
            runs.append((i, j - i))
            i = j
        else:
            i += 4
    return runs


def is_return(w):
    """`jr rs` or a `j` tail call -- how a MIPS function ends, and what padding follows.

    `jr $zero` is excluded, and that exclusion is the whole rule working or not: the word
    0x00000008 *is* a well-formed `jr $zero`, no compiler emits one, and it is a very common data
    value. Accepting it was what let the scan mistake Ocarina's `{pointer, length}` table at
    0x800067c8 for the end of a function.
    """
    if (w & 0xFC1FFFFF) == 0x08:
        return ((w >> 21) & 0x1F) != 0
    return (w >> 26) == 0x02 and (w & 0x03FFFFFF) != 0


def padding_runs(rom):
    """Zero runs that are inter-function alignment padding, rather than zeros that mean something.

    "A run of zeros bounded by non-zeros" is not enough and the difference is not cosmetic. Ten
    segments chained across every run that passed that test black-screened Ocarina; two segments
    across the runs that pass this one boot and run the engine. The runs it rejected were inside a
    table of `{pointer, length}` records -- 80006720/00000020, 80006d30/00000008 -- whose zero
    fields look exactly like padding from the outside.

    What tells them apart is what comes immediately before. Real padding follows a function's last
    instruction, so a `jr`/`j` sits one or two words back -- two when the delay slot holds a real
    instruction, one when it is a `nop` and the run therefore swallowed it. Measured over the
    15-ROM shelf this keeps every run that has been booted successfully and drops 37 of Ocarina's
    39 candidates, including all four the chain died on.
    """
    out = []
    for off, length in zero_runs(rom):
        if not (32 <= length <= GAP_MAX_BYTES):
            continue
        if any(is_return(struct.unpack_from(">I", rom, off - k)[0]) for k in (4, 8)):
            out.append((off, length))
    return out


def place(rom, entry, n_words):
    """Lay @p n_words of engine across as many padding runs as it takes, lowest first.

    One run holds about six cheats: Ocarina's is 108 bytes, which is 27 words, less four for the
    guards and four for the tail. That is enough for most single cheats and not enough for a
    multi-line group -- Ocarina's "Infinite Big Key, Small Keys, Compass & Map" alone is nineteen
    lines. So the engine is allowed to be discontiguous: each segment but the last ends with `j`
    into the next and its delay slot, costing two words a hop.

    Chaining spends the same *kind* of space rather than a riskier kind, which is the point. Every
    segment executes on every exception forever, so all of them have to be permanently-safe RAM,
    and inter-function alignment padding in resident boot code is the only space that qualifies.
    Lowest-first means the safest runs get used before the engine has to reach for anything else,
    and a one-cheat selection never uses more than one.

    @return a list of (rom_off, words) in execution order, or None if the image has nowhere.
    """
    segs, left = [], n_words
    for off, length in padding_runs(rom):
        cap = length // 4 - 2 * GAP_GUARD_WORDS
        at = off + GAP_GUARD_WORDS * 4
        if left <= cap:
            segs.append((at, left))
            return segs
        if cap >= 3:                       # a hop costs two, so fewer than three buys nothing
            segs.append((at, cap - 2))
            left -= cap - 2
    return None


def chain(segs, entry, words):
    """Turn the placement into (rom_off, word) writes, with the jumps between segments filled in."""
    out, k = [], 0
    for i, (at, n) in enumerate(segs):
        for j in range(n):
            out.append((at + j * 4, words[k]))
            k += 1
        if i + 1 < len(segs):
            nxt = entry + (segs[i + 1][0] - WINDOW_OFF)
            out.append((at + n * 4, i_j(nxt)))
            out.append((at + n * 4 + 4, i_nop()))
    return out


def emit_engine(codes, word0, word1, marker):
    """The handler, as it will sit in RDRAM.

    Only $k0 and $k1 are touched: at exception entry those two are the kernel's scratch registers
    and libultra's `__osException` clobbers them itself, so nothing has to be saved. Every cheat
    address is KSEG0, which is unmapped, so no store here can take a TLB miss inside an exception
    -- the one way this code could turn a working game into a reset loop.
    """
    words, asm = [], []

    if marker is not None:
        # A store to VI_X_SCALE, which the game rewrites only on a mode change but this handler
        # rewrites on every exception. Nothing about the game changes; the picture's horizontal
        # scale does, visibly, which is how "the handler ran" gets told apart from "the game
        # booted and the handler was never reached" in a screenshot.
        hi, lo = split_addr(0xA4400030)
        words += [i_lui(REG_K0, hi), i_ori(REG_K1, REG_ZERO, marker), i_sw(REG_K1, lo, REG_K0)]
        asm += ["lui   $k0, 0x%04x" % hi,
                "ori   $k1, $zero, 0x%04x" % marker,
                "sw    $k1, 0x%04x($k0)      ; VI_X_SCALE marker" % lo]

    for addr, value, width16 in codes:
        hi, lo = split_addr(addr)
        words += [i_lui(REG_K0, hi), i_ori(REG_K1, REG_ZERO, value),
                  (i_sh if width16 else i_sb)(REG_K1, lo, REG_K0)]
        asm += ["lui   $k0, 0x%04x" % hi,
                "ori   $k1, $zero, 0x%04x" % value,
                "%-5s $k1, 0x%04x($k0)      ; %08x <- %0*x"
                % ("sh" if width16 else "sb", lo, addr, 4 if width16 else 2, value)]

    # The tail is the preamble's own two words, verbatim. __osException is entered with $k0
    # holding its own address, exactly as the unpatched game leaves it.
    words += [word0, word1, i_jr(REG_K0), i_nop()]
    asm += ["lui   $k0, 0x%04x            ; original preamble word 0" % (word0 & 0xFFFF),
            "addiu $k0, $k0, 0x%04x       ; original preamble word 1" % (word1 & 0xFFFF),
            "jr    $k0", "nop"]
    return words, asm


def parse_code(text):
    """One GameShark line, `AAAAAAAA VVVV`, as the engine will apply it.

    The accepted types are the unconditional writes only -- 0x80/0x81 cached and 0xA0/0xA1
    uncached, the same four `cheats.c` treats as a plain store. Conditionals and repeaters need
    branches this handler does not emit, and are refused rather than silently dropped.
    """
    parts = text.replace(":", " ").split()
    if len(parts) != 2:
        raise SystemExit("code %r is not `AAAAAAAA VVVV`" % text)
    raw, value = int(parts[0], 16), int(parts[1], 16)
    kind = raw >> 24
    if kind not in (0x80, 0x81, 0xA0, 0xA1):
        raise SystemExit("code type %02X is not an unconditional write; only 80/81/A0/A1 fit"
                         % kind)
    width16 = bool(kind & 0x01)
    base = 0xA0000000 if (kind & 0x20) else 0x80000000
    return base | (raw & 0x007FFFFF), value & (0xFFFF if width16 else 0xFF), width16


def report_gaps(rom, entry):
    print("gap candidates in ROM [0x%06x, 0x%06x)  entry %08x\n" % (
        WINDOW_OFF, WINDOW_OFF + WINDOW_LEN, entry))
    print("  %-10s %-10s %7s  %-9s %-9s %s"
          % ("rom", "ram", "bytes", "-8", "-4", "verdict"))
    keep = {off for off, _ in padding_runs(rom)}
    for off, length in zero_runs(rom):
        if length < 32:
            continue
        w8, w4 = (struct.unpack_from(">I", rom, off - k)[0] for k in (8, 4))
        why = ("padding" if off in keep else
               "too big: a data void, not padding" if length > GAP_MAX_BYTES else
               "no function ends here: these zeros mean something")
        print("  0x%08x %08x %7d  %08x  %08x  %s"
              % (off, entry + (off - WINDOW_OFF), length, w8, w4, why))


def check(path):
    rom = preamblescan.normalise(open(path, "rb").read(WINDOW_OFF + WINDOW_LEN))
    if rom is None:
        raise SystemExit("%s is not a ROM" % path)
    cic, header_entry, entry = identify(rom)
    h1, h2 = struct.unpack_from(">II", rom, 0x10)
    g1, g2 = romcrc.crc(rom, cic)
    hits = preamblescan.scan(rom)
    print("%s\n  cic %s  entry %08x (header %08x)" % (path, cic, entry, header_entry))
    print("  crc header %08x %08x  computed %08x %08x  %s"
          % (h1, h2, g1, g2, "ok" if (h1, h2) == (g1, g2) else "MISMATCH"))
    for off, w0, w1, target in hits:
        ram = entry + (off - WINDOW_OFF)
        print("  preamble rom+%06x ram %08x -> %08x  %s"
              % (off, ram, target, preamblescan.verdict(ram, target)))
    return 0 if (h1, h2) == (g1, g2) else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom")
    ap.add_argument("-o", "--out", help="where to write the patched image")
    ap.add_argument("--code", action="append", default=[], metavar="'AAAAAAAA VVVV'",
                    help="a GameShark write; repeatable")
    ap.add_argument("--marker", nargs="?", const=0x0100, type=lambda s: int(s, 0), default=None,
                    metavar="XSCALE",
                    help="also store this to VI_X_SCALE every exception, so a screenshot shows "
                         "whether the handler ran at all")
    ap.add_argument("--gap-rom", type=lambda s: int(s, 0),
                    help="place the engine at this ROM offset instead of scanning")
    ap.add_argument("--accept-odd", action="store_true",
                    help="also accept a preamble whose __osException is not exactly +16 away")
    ap.add_argument("--list-gaps", action="store_true")
    ap.add_argument("--check", action="store_true", help="verify an already-patched image")
    args = ap.parse_args()

    if args.check:
        return check(args.rom)

    raw = open(args.rom, "rb").read()
    rom = preamblescan.normalise(raw)
    if rom is None:
        raise SystemExit("%s is not a ROM" % args.rom)
    cic, header_entry, entry = identify(rom)

    if args.list_gaps:
        report_gaps(rom, entry)
        return 0

    if not args.out:
        raise SystemExit("give -o OUT.z64, or --list-gaps / --check")

    # The gate rompatch.c enforces and this must too: an image whose header already disagrees
    # with its contents is one we do not understand well enough to edit.
    h1, h2 = struct.unpack_from(">II", rom, 0x10)
    g1, g2 = romcrc.crc(rom, cic)
    if (h1, h2) != (g1, g2):
        raise SystemExit("header says %08x %08x, image computes %08x %08x -- refusing to patch"
                         % (h1, h2, g1, g2))

    # "real" means the target is exactly +16, which is where libultra links __osException in the
    # builds most of the shelf uses. It is not the only build: 1080 Snowboarding and Harvest Moon
    # 64 both carry a preamble whose target is +212, the same number in two unrelated games, which
    # is a variant rather than a coincidence. --accept-odd takes any forward target inside 4 KB,
    # which still rejects GoldenEye's 0x700101A0 -- not a KSEG0 address, so not an address.
    # Relaxing it is a guess until the ROM has been booted; that is what tools/aresshot.sh is for.
    def usable(h):
        ram, target = entry + (h[0] - WINDOW_OFF), h[3]
        if preamblescan.verdict(ram, target) == "real":
            return True
        return args.accept_odd and 0x80000000 <= target < 0x80800000 \
            and 0 < target - ram <= 0x1000

    hits = [h for h in preamblescan.scan(rom) if usable(h)]
    if not hits:
        raise SystemExit("no __osExceptionPreamble in the window")
    pre_off, word0, word1, target = hits[0]
    pre_ram = entry + (pre_off - WINDOW_OFF)

    codes = [parse_code(c) for c in args.code]
    words, asm = emit_engine(codes, word0, word1, args.marker)

    if args.gap_rom is not None:
        segs = [(args.gap_rom, len(words))]
    else:
        segs = place(rom, entry, len(words))
        if segs is None:
            raise SystemExit("no padding runs holding %d words in the window" % len(words))
    gap_off = segs[0][0]
    engine_ram = entry + (gap_off - WINDOW_OFF)

    out = bytearray(rom)
    for off, w in chain(segs, entry, words):
        struct.pack_into(">I", out, off, w)
    hi, lo = split_addr(engine_ram)
    struct.pack_into(">I", out, pre_off + 0, i_lui(REG_K0, hi))
    struct.pack_into(">I", out, pre_off + 4, i_addiu(REG_K0, REG_K0, lo))

    n1, n2 = romcrc.crc(bytes(out), cic)
    struct.pack_into(">II", out, 0x10, n1, n2)

    # exist_ok is not enough under the sandbox this runs in: mkdir fails EEXIST while isdir()
    # comes back False, and makedirs re-raises on exactly that combination. If the directory
    # really is missing, the open below says so in one line instead of a traceback.
    try:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    except OSError:
        pass
    with open(args.out, "wb") as f:
        f.write(out)

    print("%s\n  -> %s" % (args.rom, args.out))
    print("  cic %s  entry %08x  preamble rom+%06x ram %08x -> was %08x"
          % (cic, entry, pre_off, pre_ram, target))
    print("  engine rom+%06x ram %08x  %d words across %d segment%s"
          % (gap_off, engine_ram, len(words), len(segs), "" if len(segs) == 1 else "s"))
    print("  crc %08x %08x -> %08x %08x" % (h1, h2, n1, n2))
    print("\n  preamble now:")
    print("    %08x  lui   $k0, 0x%04x" % (i_lui(REG_K0, hi), hi))
    print("    %08x  addiu $k0, $k0, 0x%04x" % (i_addiu(REG_K0, REG_K0, lo), lo))
    print("    %08x  jr    $k0" % struct.unpack_from(">I", out, pre_off + 8))
    print("    %08x  nop" % struct.unpack_from(">I", out, pre_off + 12))
    print("\n  engine:")
    k = 0
    for i, (at, n) in enumerate(segs):
        for j in range(n):
            print("    %08x  %08x  %s"
                  % (entry + (at + j * 4 - WINDOW_OFF), words[k], asm[k]))
            k += 1
        if i + 1 < len(segs):
            nxt = entry + (segs[i + 1][0] - WINDOW_OFF)
            print("    %08x  %08x  j     0x%08x        ; on into the next padding run"
                  % (entry + (at + n * 4 - WINDOW_OFF), i_j(nxt), nxt))
            print("    %08x  %08x  nop"
                  % (entry + (at + n * 4 + 4 - WINDOW_OFF), i_nop()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
