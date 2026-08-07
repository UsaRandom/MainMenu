#!/usr/bin/env python3
"""
Look for libultra's __osExceptionPreamble in a ROM, the way the cheat patcher does.

    tools/preamblescan.py /Volumes/SC64/roms/n64/*.z64

## Why this exists

The cheat engine cannot hook itself through the watch exception on the M64 -- the CPU holds
WatchLo and never delivers the trap (AUDIT.md 1af) -- so the patcher instead scans the megabyte
IPL3 loaded, finds the four instructions osInitialize copies onto every exception vector, and
rewrites them to install our hook. On hardware the engine then did not run (AUDIT.md 1ao), and
"the scan missed" and "the scan hit and the hook was bypassed" are two completely different
problems with one symptom.

That scan happens on the console after the menu is gone. This is the same scan, run on a PC, over
the same bytes -- IPL3 copies ROM offset 0x1000 onwards verbatim to the entry address, so
ROM[0x1000 + k] is exactly the RDRAM word the patcher will look at. The answer arrives in seconds
instead of a launch, and it can be asked of a whole shelf of ROMs at once rather than one.

## The pattern

Four instructions. Only the two address halves vary between games, which is what makes it
findable at all:

    lui   $k0, %hi(__osException)     0x3C1A____
    addiu $k0, $k0, %lo(__osException) 0x275A____
    jr    $k0                          0x03400008
    nop                                0x00000000

Those encodings are duplicated from src/boot/vr4300_asm.h and that duplication is the one risk
here: a tool that agrees with itself and not with the console is worse than no tool. The host
suite asserts the C macros still produce these exact words, so a drift is a build failure rather
than a quiet disagreement -- see tools/hosttest/test_preamble.c.
"""

import argparse
import os
import struct
import sys
import zlib

# See the module docstring. Cross-checked against the C by tools/hosttest/test_preamble.c.
LUI_K0_HI16 = 0x3C1A
ADDIU_K0_HI16 = 0x275A
JR_K0 = 0x03400008
NOP = 0x00000000

# What IPL3 copies, and where from. The patcher scans [entry, entry + 1 MB); IPL3 filled that
# from ROM offset 0x1000, so this is the same window seen from the other side.
IPL3_LOAD_OFFSET = 0x1000
IPL3_LOAD_BYTES = 0x100000

Z64_MAGIC = 0x80371240      # big-endian, which is what the console reads
N64_MAGIC = 0x40123780      # byte-swapped
V64_MAGIC = 0x37804012      # half-word swapped

# IPL3 does not always load to the entry point in the header. Two of the CICs shift it, and
# rom_info.c's fix_boot_address() is where the console applies the same two numbers. Getting this
# wrong does not look like an error: it looks like a preamble whose target is a megabyte away from
# it, which is exactly how five of the twenty-four ROMs on the reference card first read.
#
# Identified by CRC32 of the 4,032-byte IPL3 rather than by the console's seeded checksum, because
# the CRC is a table lookup and the checksum is a hundred lines of 64-bit mixing. They agree on
# everything that matters here; if they ever did not, the console's answer is the real one.
IPL3_CRC = {
    0x6170A4A1: ("6101",      0),
    0x90BB6CB5: ("6102/7101", 0),
    0x0B050EE0: ("6103/7103", -0x100000),
    0x98BC2C86: ("6105/7105", 0),
    0xACC8580A: ("6106/7106", -0x200000),
    0x009E9EA3: ("7102",      0),
}


def normalise(data):
    """Return the ROM as big-endian bytes, or None if it is not a ROM at all."""
    if len(data) < 4096:
        return None
    magic = struct.unpack_from(">I", data, 0)[0]
    if magic == Z64_MAGIC:
        return data
    if magic == V64_MAGIC:
        b = bytearray(data)
        b[0::2], b[1::2] = data[1::2], data[0::2]
        return bytes(b)
    if magic == N64_MAGIC:
        b = bytearray(len(data))
        b[0::4], b[1::4], b[2::4], b[3::4] = data[3::4], data[2::4], data[1::4], data[0::4]
        return bytes(b)
    return None


def scan(data):
    """Every preamble-shaped run of four words in the window IPL3 loads."""
    end = min(len(data), IPL3_LOAD_OFFSET + IPL3_LOAD_BYTES)
    hits = []
    for off in range(IPL3_LOAD_OFFSET, end - 16 + 1, 4):
        w0, w1, w2, w3 = struct.unpack_from(">IIII", data, off)
        if (w0 >> 16) == LUI_K0_HI16 and (w1 >> 16) == ADDIU_K0_HI16 \
                and w2 == JR_K0 and w3 == NOP:
            # %lo is sign-extended by addiu, which is why %hi is pre-biased in the compiler's
            # output. Reconstructing it the same way gives the address the game will jump to.
            lo = w1 & 0xFFFF
            if lo & 0x8000:
                lo -= 0x10000
            hits.append((off, w0, w1, ((w0 & 0xFFFF) << 16) + lo))
    return hits


def verdict(ram, target):
    """How much the match looks like a real preamble rather than a coincidence.

    The pattern fixes eight of sixteen bytes and half the rest, so a megabyte of arbitrary data
    can produce one by accident -- and on the reference card two of twenty-four ROMs do. The
    giveaway is the target: libultra links __osException immediately after the stub that jumps to
    it, so a genuine match points exactly sixteen bytes forward. A target that is not even a KSEG0
    address is not an address at all.

    This matters more than it looks. The patcher takes the FIRST match and rewrites two words of
    live game code at it. On a bogus match that is two words of something else.
    """
    if not (0x80000000 <= target < 0x80800000):
        return "BOGUS"
    return "real" if target - ram == 16 else "odd"


def note(ram, target, count):
    bits = []
    d = target - ram
    if d != 16:
        bits.append("target %+d away, not the usual +16" % d)
    if count > 1:
        bits.append("%d matches, patcher takes the first" % count)
    return "; ".join(bits)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("roms", nargs="+")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="print every match, not just the first")
    args = ap.parse_args()

    found = missed = unreadable = 0
    for path in sorted(args.roms):
        name = os.path.basename(path)
        try:
            with open(path, "rb") as f:
                data = f.read(IPL3_LOAD_OFFSET + IPL3_LOAD_BYTES)
        except OSError as e:
            print("%-44s UNREADABLE  %s" % (name[:44], e))
            unreadable += 1
            continue

        rom = normalise(data)
        if rom is None:
            print("%-44s NOT A ROM" % name[:44])
            unreadable += 1
            continue

        crc = zlib.crc32(rom[0x40:0x1000]) & 0xFFFFFFFF
        cic, shift = IPL3_CRC.get(crc, ("?%08x" % crc, 0))
        entry = struct.unpack_from(">I", rom, 8)[0] + shift

        hits = scan(rom)
        if not hits:
            print("%-40s %-9s MISS   nothing preamble-shaped in the first MB"
                  % (name[:40], cic))
            missed += 1
            continue

        found += 1
        off, w0, w1, target = hits[0]
        ram = entry + (off - IPL3_LOAD_OFFSET)
        print("%-40s %-9s %-6s %08x -> %08x  %s"
              % (name[:40], cic, verdict(ram, target), ram, target,
                 note(ram, target, len(hits))))
        if args.verbose:
            for off, w0, w1, target in hits:
                ram = entry + (off - IPL3_LOAD_OFFSET)
                print("      rom+%06x ram %08x  %08x %08x  -> %08x  %s"
                      % (off, ram, w0, w1, target, verdict(ram, target)))

    total = found + missed + unreadable
    print("\n%d ROMs: %d with a preamble, %d without, %d unreadable"
          % (total, found, missed, unreadable))
    return 0 if missed == 0 and unreadable == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
