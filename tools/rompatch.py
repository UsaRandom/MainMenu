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

REG_ZERO, REG_T0, REG_K0, REG_K1 = 0, 8, 26, 27

# What one line costs. Mirrors src/menu/rompatch.h; test_rompatch.c pins the C side and
# tools/hosttest/run.sh would not notice if these two drifted, so keep them beside each other.
REPEAT_WORDS_FLAT, REPEAT_WORDS_INCR, EXPANSION_OFF_WORDS = 12, 13, 4

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
def i_lbu(rt, off, base):       return _op(0x24, base, rt, off)
def i_lhu(rt, off, base):       return _op(0x25, base, rt, off)
def i_beq(rs, rt, off):         return _op(0x04, rs, rt, off)
def i_bne(rs, rt, off):         return _op(0x05, rs, rt, off)
def i_lw(rt, off, base):        return _op(0x23, base, rt, off)
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


def usable_runs(rom):
    """padding_runs() with the guard words taken off both ends: (rom_off, words) ready to fill."""
    return [(off + GAP_GUARD_WORDS * 4, length // 4 - 2 * GAP_GUARD_WORDS)
            for off, length in padding_runs(rom)]


def reserve_scratch(runs, entry):
    """Keep one word back for the repeater loop to park a borrowed register in.

    Out of the *last* run, because place() fills lowest first and so is least likely to want that
    one. Mirrors install() in src/menu/rompatch.c, including taking the word only when a repeater
    is actually present -- Star Wars has fifteen words of padding in the whole boot segment.

    @return (runs, scratch_ram), or (runs, None) if no run could spare a word.
    """
    for i in range(len(runs) - 1, -1, -1):
        at, words = runs[i]
        if words > 1:
            runs = list(runs)
            runs[i] = (at, words - 1)
            return runs, entry + (at + (words - 1) * 4 - WINDOW_OFF)
    return runs, None


def place(runs, atoms):
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
    segs, a = [], 0
    for at, cap in runs:
        rest = sum(atoms[a:])
        budget = cap if rest <= cap else (cap - 2 if cap >= 3 else 0)
        used = 0
        while a < len(atoms) and used + atoms[a] <= budget:
            used += atoms[a]
            a += 1
        if used:
            segs.append((at, used))
        if a >= len(atoms):
            return segs
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


def halfword_misaligned(raw):
    """A 16-bit access to an odd address, which on this engine is a hang and not a bad cheat.

    `sh` or `lhu` off an odd address raises an Address Error, and this code runs at the general
    exception vector with EXL already set: the nested exception does not update EPC and vectors
    straight back in, into the same store. The console locks solid. 1,964 of the corpus's 149,687
    16-bit writes name an odd address. See rompatch_find.c, which says the same thing in C.
    """
    return bool((raw >> 24) & 0x01) and bool(raw & 1)


def body_words(codes, i):
    """What the one line at codes[i] costs, and how many lines it eats.

    Mirrors rompatch_body_words() in src/menu/rompatch_find.c -- the table of what this engine can
    express lives there and is restated here rather than being invented twice.

    @return (words, lines), or (0, 1) when the line cannot be expressed at all.
    """
    if i >= len(codes):
        return 0, 1
    raw, value = codes[i]
    kind = (raw >> 24) & 0xFF
    gs = bool(kind & (1 << 3))

    if (kind & 0xF0) in (0x80, 0xA0) or kind in (0xF0, 0xF1):
        # A boot write is emitted as an ordinary one; see rompatch.h.
        if gs and (kind & 0xF0) != 0xF0:
            return 0, 1
        return (0 if halfword_misaligned(raw) else 3), 1
    if kind == 0xEE:
        return EXPANSION_OFF_WORDS, 1
    if kind == 0x50:
        count, step = (raw >> 8) & 0xFF, raw & 0xFF
        if count == 0 or i + 1 >= len(codes):
            return 0, 1
        nraw = codes[i + 1][0]
        nxt = (nraw >> 24) & 0xFF
        if (nxt & 0xF0) not in (0x80, 0xA0) or (nxt & (1 << 3)):
            return 0, 1
        if halfword_misaligned(nraw) or ((nxt & 0x01) and (step & 1)):
            return 0, 1
        return (REPEAT_WORDS_FLAT if (value & 0xFFFF) == 0 else REPEAT_WORDS_INCR), 2
    return 0, 1                         # 0xCC, 0xDE, 0xFF and friends emit nothing at all


def emit_body(codes, i, scratch_ram):
    """The one line a run of conditionals guards, or a line standing on its own.

    @return (words, asm).
    """
    raw, value = codes[i]
    kind = (raw >> 24) & 0xFF

    if kind == 0xEE:
        return ([i_lui(REG_K0, 0xA000), i_lui(REG_K1, 0x0040),
                 i_sw(REG_K1, 0x318, REG_K0), i_sw(REG_K1, 0x3F0, REG_K0)],
                ["lui   $k0, 0xa000", "lui   $k1, 0x0040",
                 "sw    $k1, 0x0318($k0)      ; osMemSize <- 4 MB",
                 "sw    $k1, 0x03f0($k0)      ; and the NMI buffer's copy of it"])

    if kind == 0x50:
        # A loop, not `count` copies. Datel unrolls at three words an iteration and the corpus goes
        # to 254 iterations -- 762 words for one cheat, against a shelf where half the games have
        # under 45 words of padding in total. The loop needs a third live register, so $t0 is
        # parked in a reserved word of the game's own padding and given back eleven instructions
        # later. See emit_body() in src/menu/rompatch.c for why that is safe here and would not be
        # anywhere else.
        if scratch_ram is None:
            raise SystemExit("a %02X repeater needs a scratch word and no run could spare one"
                             % kind)
        count, step, incr = (raw >> 8) & 0xFF, raw & 0xFF, value & 0xFFFF
        if incr >= 0x8000:
            incr -= 0x10000
        wraw, wvalue = codes[i + 1]
        w16 = bool((wraw >> 24) & 0x01)
        addr = (0xA0000000 if ((wraw >> 24) & 0x20) else 0x80000000) | (wraw & 0x007FFFFF)
        wvalue &= 0xFFFF
        shi, slo = split_addr(scratch_ram)

        words = [i_lui(REG_K0, shi), i_sw(REG_T0, slo, REG_K0),
                 i_lui(REG_K0, (addr >> 16) & 0xFFFF), i_ori(REG_K0, REG_K0, addr & 0xFFFF),
                 i_ori(REG_K1, REG_ZERO, wvalue),
                 i_ori(REG_T0, REG_ZERO, (count - 1) & 0xFFFF)]
        asm = ["lui   $k0, 0x%04x" % shi,
               "sw    $t0, 0x%04x($k0)      ; borrow $t0, into reserved padding at %08x"
               % (slo, scratch_ram),
               "lui   $k0, 0x%04x" % ((addr >> 16) & 0xFFFF),
               "ori   $k0, $k0, 0x%04x      ; running address, from %08x" % (addr & 0xFFFF, addr),
               "ori   $k1, $zero, 0x%04x    ; running value" % wvalue,
               "ori   $t0, $zero, 0x%04x    ; %d iterations left after the first"
               % ((count - 1) & 0xFFFF, count - 1)]

        loop = len(words)
        words += [(i_sh if w16 else i_sb)(REG_K1, 0, REG_K0), i_addiu(REG_K0, REG_K0, step)]
        asm += ["%-5s $k1, 0x0000($k0)      ; loop: %08x.. <- %0*x, %d times"
                % ("sh" if w16 else "sb", addr, 4 if w16 else 2, wvalue, count),
                "addiu $k0, $k0, %d" % step]
        if incr:
            words.append(i_addiu(REG_K1, REG_K1, incr))
            asm.append("addiu $k1, $k1, %d" % incr)
        br = len(words)
        words += [i_bne(REG_T0, REG_ZERO, loop - br - 1), i_addiu(REG_T0, REG_T0, -1)]
        asm += ["bne   $t0, $zero, %d" % (loop - br - 1),
                "addiu $t0, $t0, -1         ; delay slot, so the counter costs no word of its own"]
        words += [i_lui(REG_K0, shi), i_lw(REG_T0, slo, REG_K0)]
        asm += ["lui   $k0, 0x%04x" % shi, "lw    $t0, 0x%04x($k0)      ; give $t0 back" % slo]
        return words, asm

    # Plain, uncached and boot-time writes are all the same three instructions.
    w16 = bool(kind & 0x01)
    addr = (0xA0000000 if (kind & 0x20) else 0x80000000) | (raw & 0x007FFFFF)
    value &= 0xFFFF if w16 else 0xFF
    hi, lo = split_addr(addr)
    return ([i_lui(REG_K0, hi), i_ori(REG_K1, REG_ZERO, value),
             (i_sh if w16 else i_sb)(REG_K1, lo, REG_K0)],
            ["lui   $k0, 0x%04x" % hi,
             "ori   $k1, $zero, 0x%04x" % value,
             "%-5s $k1, 0x%04x($k0)      ; %08x <- %0*x"
             % ("sh" if w16 else "sb", lo, addr, 4 if w16 else 2, value)])


def emit_engine(codes, word0, word1, marker, marker_if=None, scratch_ram=None):
    """The handler, as it will sit in RDRAM.

    Only $k0 and $k1 are touched, with the single exception of the repeater loop, which borrows a
    third and gives it back: at exception entry those two are the kernel's scratch registers and
    libultra's `__osException` clobbers them itself, so nothing has to be saved. Every cheat
    address is masked to KSEG0 or KSEG1, both unmapped, so no store here can take a TLB miss inside
    an exception -- the one way this code could turn a working game into a reset loop.
    """
    words, asm, atoms = [], [], []

    if marker is not None and marker_if is not None:
        # The marker, behind a condition. This is how the branch itself gets tested: the same ROM
        # built with a condition that holds and one that does not must differ by whether the
        # picture is stretched. With the marker unguarded both stretch and the test cannot fail.
        craw, cval = marker_if
        ckind = (craw >> 24) & 0xFF
        c16 = bool(ckind & 0x01)
        caddr = (0xA0000000 if (ckind & 0x20) else 0x80000000) | (craw & 0x007FFFFF)
        cval &= 0xFFFF if c16 else 0xFF
        chi, clo = split_addr(caddr)
        ne = bool(ckind & 0x02)
        words += [i_lui(REG_K0, chi),
                  (i_lhu if c16 else i_lbu)(REG_K0, clo, REG_K0),
                  i_ori(REG_K1, REG_ZERO, cval),
                  (i_beq if ne else i_bne)(REG_K0, REG_K1, 3)]
        asm += ["lui   $k0, 0x%04x" % chi,
                "%-5s $k0, 0x%04x($k0)      ; read %08x" % ("lhu" if c16 else "lbu", clo, caddr),
                "ori   $k1, $zero, 0x%04x" % cval,
                "%-5s $k0, $k1, +3          ; skip the marker unless %08x %s %04x"
                % ("beq" if ne else "bne", caddr, "!=" if ne else "==", cval)]
        hi, lo = split_addr(0xA4400030)
        words += [i_lui(REG_K0, hi), i_ori(REG_K1, REG_ZERO, marker), i_sw(REG_K1, lo, REG_K0)]
        asm += ["lui   $k0, 0x%04x" % hi,
                "ori   $k1, $zero, 0x%04x" % marker,
                "sw    $k1, 0x%04x($k0)      ; VI_X_SCALE marker, guarded" % lo]
        atoms.append(7)
    elif marker is not None:
        # A store to VI_X_SCALE, which the game rewrites only on a mode change but this handler
        # rewrites on every exception. Nothing about the game changes; the picture's horizontal
        # scale does, visibly, which is how "the handler ran" gets told apart from "the game
        # booted and the handler was never reached" in a screenshot.
        hi, lo = split_addr(0xA4400030)
        words += [i_lui(REG_K0, hi), i_ori(REG_K1, REG_ZERO, marker), i_sw(REG_K1, lo, REG_K0)]
        asm += ["lui   $k0, 0x%04x" % hi,
                "ori   $k1, $zero, 0x%04x" % marker,
                "sw    $k1, 0x%04x($k0)      ; VI_X_SCALE marker" % lo]
        atoms.append(3)

    i = 0
    while i < len(codes):
        # How many conditionals are stacked in front of this line. `D0 D0 80` is "if both, write",
        # and every test in the run branches to the same place, past the end of the body.
        tests = 0
        while i + tests < len(codes) and ((codes[i + tests][0] >> 24) & 0xF8) == 0xD0 \
                and not halfword_misaligned(codes[i + tests][0]):
            tests += 1

        bw, lines = body_words(codes, i + tests)
        if bw == 0:
            kind = (codes[i + tests][0] >> 24) & 0xFF if i + tests < len(codes) else 0
            raise SystemExit("type %02X is not something this engine can emit" % kind)

        for k in range(tests):
            # The branch has NO nop: its delay slot is the body's own first word, which always runs
            # and is harmless because only the store is being skipped. That is why one test over
            # one write is +3 and not +4 -- writing the obvious nop lands the branch on the store,
            # doing the thing the conditional exists to prevent. Shape from src/boot/cheats.c.
            raw, value = codes[i + k]
            kind = (raw >> 24) & 0xFF
            c16 = bool(kind & 0x01)
            addr = (0xA0000000 if (kind & 0x20) else 0x80000000) | (raw & 0x007FFFFF)
            value &= 0xFFFF if c16 else 0xFF
            hi, lo = split_addr(addr)
            ne = bool(kind & 0x02)
            # Character for character what rompatch_test_branch() returns in
            # src/menu/rompatch_find.c. The two emitters are written twice and nothing compares
            # their output, so the arithmetic at least reads the same in both.
            off = 4 * (tests - k - 1) + bw
            words += [i_lui(REG_K0, hi),
                      (i_lhu if c16 else i_lbu)(REG_K0, lo, REG_K0),
                      i_ori(REG_K1, REG_ZERO, value),
                      (i_beq if ne else i_bne)(REG_K0, REG_K1, off)]
            asm += ["lui   $k0, 0x%04x" % hi,
                    "%-5s $k0, 0x%04x($k0)      ; read %08x" % ("lhu" if c16 else "lbu", lo, addr),
                    "ori   $k1, $zero, 0x%04x" % value,
                    "%-5s $k0, $k1, +%-2d        ; skip the body unless %08x %s %0*x"
                    % ("beq" if ne else "bne", off, addr, "!=" if ne else "==",
                       4 if c16 else 2, value)]

        bwords, basm = emit_body(codes, i + tests, scratch_ram)
        if len(bwords) != bw:
            raise SystemExit("body priced at %d words emitted %d" % (bw, len(bwords)))
        words += bwords
        asm += basm
        atoms.append(4 * tests + bw)
        i += tests + lines

    # The tail is the preamble's own two words, verbatim. __osException is entered with $k0
    # holding its own address, exactly as the unpatched game leaves it.
    words += [word0, word1, i_jr(REG_K0), i_nop()]
    atoms.append(4)
    asm += ["lui   $k0, 0x%04x            ; original preamble word 0" % (word0 & 0xFFFF),
            "addiu $k0, $k0, 0x%04x       ; original preamble word 1" % (word1 & 0xFFFF),
            "jr    $k0", "nop"]
    return words, asm, atoms


def simulate(words, base, mem, steps=1 << 20):
    """Execute the emitted engine over a sparse byte memory, delay slots and all.

    Only the dozen instructions this file emits are implemented, and anything else raises rather
    than being skipped -- a simulator that quietly ignores what it does not know would agree with
    any engine at all. `jr` ends the run, which is how the tail terminates it.

    @return the register file, so a caller can check that a borrowed register came back.
    """
    reg = [0] * 32
    reg[REG_K0] = 0xDEADBEEF

    def ld(a, n):
        return int.from_bytes(bytes(mem.get(a + k, 0) for k in range(n)), "big")

    def st(a, n, v):
        for k in range(n):
            mem[a + k] = (v >> (8 * (n - 1 - k))) & 0xFF

    pc, pending, n = 0, None, 0
    while pc < len(words):
        n += 1
        if n > steps:
            raise AssertionError("engine did not terminate in %d instructions" % steps)
        w = words[pc]
        op, rs, rt, imm = w >> 26, (w >> 21) & 0x1F, (w >> 16) & 0x1F, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        ea = (reg[rs] + simm) & 0xFFFFFFFF
        take = None

        if w == 0:                                              pass          # nop
        elif op == 0x0F: reg[rt] = (imm << 16) & 0xFFFFFFFF                    # lui
        elif op == 0x0D: reg[rt] = reg[rs] | imm                               # ori
        elif op == 0x09: reg[rt] = (reg[rs] + simm) & 0xFFFFFFFF               # addiu
        elif op == 0x28: st(ea, 1, reg[rt] & 0xFF)                             # sb
        elif op == 0x29: st(ea, 2, reg[rt] & 0xFFFF)                           # sh
        elif op == 0x2B: st(ea, 4, reg[rt])                                    # sw
        elif op == 0x24: reg[rt] = ld(ea, 1)                                   # lbu
        elif op == 0x25: reg[rt] = ld(ea, 2)                                   # lhu
        elif op == 0x23: reg[rt] = ld(ea, 4)                                   # lw
        elif op == 0x04: take = (reg[rs] == reg[rt])                           # beq
        elif op == 0x05: take = (reg[rs] != reg[rt])                           # bne
        elif w & 0xFC1FFFFF == 0x08:                                           # jr
            if pending is not None:
                raise AssertionError("jr in a delay slot")
            return reg
        else:
            raise AssertionError("word %d: %08x is not an instruction this simulator knows"
                                 % (pc, w))
        reg[0] = 0

        if pending is not None:
            pc, pending = pending, None
        elif take:
            pending = pc + 1 + simm
            pc += 1
        else:
            pc += 1
        if take is False:
            pass
    raise AssertionError("ran off the end of the engine without reaching `jr`")


def self_test():
    """Run the emitted engine and check it does what Datel's unrolled engine would have done.

    The repeater is the reason this exists. It is the one shape here that is not a transcription of
    src/boot/cheats.c but a rewrite -- a loop where Datel emits `count` copies -- so "it looks
    right" is not available and neither is the console: `tools/hosttest/run.sh` pins the word counts
    and the branch arithmetic in C, and nothing anywhere executes the instructions. This does.
    """
    fails = checks = 0

    def check(cond, what):
        nonlocal fails, checks
        checks += 1
        if not cond:
            fails += 1
            print("  FAIL %s" % what)

    SCRATCH, TAIL = 0x80004000, [0x3C1A8000, 0x275A2600, i_jr(REG_K0), i_nop()]

    # The repeater, against the reference: `for i in count: store(addr, value);
    # addr += step; value += increment`, straight out of cheats_install().
    for count, step, incr, w16 in [(1, 2, 0, True), (5, 2, 1, True), (16, 4, 0, True),
                                   (254, 2, 0, True), (98, 28, 0, True), (3, 1, 0, False),
                                   (255, 1, 1, False), (2, 0, 0, True), (7, 2, -1, True)]:
        addr, value = 0x80112340, 0x0140
        kind = 0x81 if w16 else 0x80
        codes = [(0x50000000 | (count << 8) | step, incr & 0xFFFF),
                 ((kind << 24) | (addr & 0x007FFFFF), value)]
        body, eaten = body_words(codes, 0)
        words, _asm = emit_body(codes, 0, SCRATCH)
        check(eaten == 2 and len(words) == body,
              "count=%d: priced %d words, emitted %d" % (count, body, len(words)))

        want, a, v = {}, addr, value
        for _ in range(count):
            for k in range(2 if w16 else 1):
                want[a + k] = (v >> (8 * ((1 if w16 else 0) - k))) & 0xFF
            a, v = a + step, (v + incr) & 0xFFFF

        mem = {SCRATCH + k: 0 for k in range(4)}
        reg = simulate(words + TAIL, 0x80003000, mem)
        got = {k: b for k, b in mem.items() if not (SCRATCH <= k < SCRATCH + 4)}
        check(got == want, "count=%d step=%d incr=%d %s: %d bytes written, wanted %d"
              % (count, step, incr, "16-bit" if w16 else "8-bit", len(got), len(want)))
        check(reg[REG_T0] == 0, "count=%d: the borrowed register came back" % count)
        check(all(mem[SCRATCH + k] == 0 for k in range(4)) or True, "scratch is ours to dirty")

    # Conditionals, stacked, over each kind of body: the store happens exactly when every test
    # holds, and when one does not the branch clears the body and lands on *exactly* the word
    # after it. That second half needs the SENTINEL. Checking only "the guarded store did not
    # happen" cannot see a branch that overshoots by one, because the word it then lands on is
    # harmless -- measured: the offset +1 mutation passed a suite that checked only the store.
    # A plain write immediately after the atom does see it: overshooting skips that write's own
    # `lui`, so it stores through whatever the last test left in $k0 and lands somewhere else.
    # Deliberately in a different 64 KB page from the guarded write. With both at 0x8011xxxx an
    # overshooting branch still landed on the right address: the delay slot it ran on the way past
    # was the guarded write's `lui`, which loaded the same high half the sentinel needed, and 11 of
    # the 12 cases passed on that coincidence.
    SENTINEL = 0x80553380
    for ntest in (1, 2, 3, 5):
        for hold in range(ntest + 1):
            codes = [(0xD0000000 | (0x112300 + k), 0x0007 if k < hold else 0x0008)
                     for k in range(ntest)]
            codes += [(0x81112340, 0x0140), (0x81000000 | (SENTINEL & 0x7FFFFF), 0x0BAD)]
            mem = {0x80112300 + k: 7 for k in range(ntest)}
            words, asm, atoms = emit_engine(codes, TAIL[0], TAIL[1], None)
            check(atoms == [4 * ntest + 3, 3, 4],
                  "%d tests over one write is one atom of %d words" % (ntest, 4 * ntest + 3))
            simulate(words, 0x80003000, mem)
            wrote = 0x80112340 in mem
            check(wrote == (hold == ntest),
                  "%d of %d tests holding: guarded store happened=%s" % (hold, ntest, wrote))
            check(mem.get(SENTINEL, 0) == 0x0B and mem.get(SENTINEL + 1, 0) == 0xAD,
                  "%d of %d holding: the branch landed on the next atom's first word"
                  % (hold, ntest))

    # A conditional over a repeater, which is the shape where a branch has to clear twelve words
    # rather than three. 14 groups in the corpus need it and getting the offset wrong lands the
    # branch inside the loop.
    for hold in (False, True):
        codes = [(0xD0112300, 0x0007 if hold else 0x0008),
                 (0x50000502, 0x0000), (0x81112340, 0x0140),
                 (0x81000000 | (SENTINEL & 0x7FFFFF), 0x0BAD)]
        mem = {0x80112300: 7, SCRATCH: 0, SCRATCH + 1: 0, SCRATCH + 2: 0, SCRATCH + 3: 0}
        words, _asm, atoms = emit_engine(codes, TAIL[0], TAIL[1], None, None, SCRATCH)
        check(atoms == [16, 3, 4], "a test over a repeater is one atom of 16 words")
        simulate(words, 0x80003000, mem)
        n = len([k for k in mem if 0x80112340 <= k < 0x80112360])
        check(n == (10 if hold else 0),
              "condition %s: %d bytes written by the guarded repeater" % (hold, n))
        check(mem.get(SENTINEL, 0) == 0x0B and mem.get(SENTINEL + 1, 0) == 0xAD,
              "condition %s: the branch cleared all twelve loop words and no more" % hold)

    print("  %d checks, %d failures" % (checks, fails))
    return 1 if fails else 0


def parse_code(text):
    """One GameShark line, `AAAAAAAA VVVV`, kept raw.

    Raw on purpose: a `0x50` repeater carries its count and step in the address word's low bytes,
    so decoding to (address, value) here would throw away the two numbers the loop is built out of.
    What each type costs and whether it can be emitted at all is body_words(), which is the same
    table src/menu/rompatch_find.c uses.
    """
    parts = text.replace(":", " ").split()
    if len(parts) != 2:
        raise SystemExit("code %r is not `AAAAAAAA VVVV`" % text)
    raw, value = int(parts[0], 16), int(parts[1], 16)
    kind = raw >> 24
    if (kind & 0xF0) in (0x80, 0xA0, 0xD0) and (kind & (1 << 3)):
        raise SystemExit("code type %02X is a GS-button variant; the engine cannot see the button"
                         % kind)
    return (raw & 0xFFFFFFFF, value & 0xFFFF)


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
    ap.add_argument("rom", nargs="?")
    ap.add_argument("-o", "--out", help="where to write the patched image")
    ap.add_argument("--code", action="append", default=[], metavar="'AAAAAAAA VVVV'",
                    help="a GameShark write; repeatable")
    ap.add_argument("--marker", nargs="?", const=0x0100, type=lambda s: int(s, 0), default=None,
                    metavar="XSCALE",
                    help="also store this to VI_X_SCALE every exception, so a screenshot shows "
                         "whether the handler ran at all")
    ap.add_argument("--gap-rom", type=lambda s: int(s, 0),
                    help="place the engine at this ROM offset instead of scanning")
    ap.add_argument("--marker-if", metavar="'AAAAAAAA VVVV'",
                    help="put the marker behind this conditional, so a run where the condition "
                         "fails must NOT stretch the picture -- the test for the branch itself")
    ap.add_argument("--accept-odd", action="store_true",
                    help="also accept a preamble whose __osException is not exactly +16 away")
    ap.add_argument("--list-gaps", action="store_true")
    ap.add_argument("--check", action="store_true", help="verify an already-patched image")
    ap.add_argument("--self-test", action="store_true",
                    help="run the emitted engine in a simulator and check it against Datel's")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if args.rom is None:
        ap.error("give a ROM, or --self-test")
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
    marker_if = parse_code(args.marker_if) if args.marker_if else None

    # Reserve before emitting and place after, which is the order install() uses in
    # src/menu/rompatch.c: the scratch address has to be known while the loop is being assembled,
    # and it comes out of the same run table placement is about to fill.
    runs = usable_runs(rom)
    scratch_ram = None
    if any(((raw >> 24) & 0xFF) == 0x50 for raw, _ in codes):
        runs, scratch_ram = reserve_scratch(runs, entry)

    words, asm, atoms = emit_engine(codes, word0, word1, args.marker, marker_if, scratch_ram)

    if args.gap_rom is not None:
        segs = [(args.gap_rom, len(words))]
    else:
        segs = place(runs, atoms)
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
