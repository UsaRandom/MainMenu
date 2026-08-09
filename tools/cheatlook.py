#!/usr/bin/env python3
"""
Ask cheats.db what a given cartridge would actually be offered, the way the console asks.

    tools/cheatlook.py build/cheats.db NEPE            # every cheat a USA Racer would see
    tools/cheatlook.py build/cheats.db NEPE --grep money
    tools/cheatlook.py build/cheats.db --rom roms/n64/*.z64    # read the key out of the ROMs

## Why this exists

Every cheat bug this project has had has been a *lookup* bug rather than an engine bug: Ocarina
V1.2 handed V1.0's addresses (AUDIT 2aa), NSMJ shadowed by NSM?, and a USA Star Wars Racer offered
"Infinite Money" at the European address. In all three the engine wrote faithfully to an address
that meant nothing in that binary, and there is no way to tell that apart from "the cheat is
broken" by looking at a television.

So this reimplements find_row() from src/cheats/cheatdb.c -- check code first, then most-specific
game code, an exact version beating the ANY sentinel and an exact four-character code beating the
three-character wildcard -- and prints what comes back. If this and the console ever disagree, this
is worse than useless, so it is a transcription and not an improvement.
"""

import argparse
import glob
import os
import struct
import sys

MAGIC = 0x4D363443              # 'M64C'
ANY_VERSION = 0xFF
HEADER, INDEX_ROW, GROUP_ROW, CODE_ROW = 32, 24, 8, 8


def load(path):
    blob = open(path, "rb").read()
    magic, ver, _r, count, index_off, blob_off, str_off, _str_len, _crc = \
        struct.unpack(">IHHIIIIII", blob[:32])
    if magic != MAGIC:
        sys.exit("%s is not a cheats.db (magic %08X)" % (path, magic))
    rows = []
    for i in range(count):
        o = index_off + i * INDEX_ROW
        cc, code, version, _flags, n, off, _size = struct.unpack(">Q4sBBHII", blob[o:o + INDEX_ROW])
        rows.append((cc, code.decode("ascii", "replace"), version, n, off))
    return blob, rows, blob_off, str_off


def find_row(rows, check_code, game_code, version):
    """cheatdb.c's find_row(), transcribed. Most specific wins, not first found."""
    if check_code:
        for r in rows:
            if r[0] == check_code:
                return r, 5
    if game_code is None:
        return None, 0
    best, best_rank = None, 0
    for r in rows:
        stored = r[1]
        wild = stored[3:4] == "?"
        if stored[:3 if wild else 4] != game_code[:3 if wild else 4]:
            continue
        if r[2] == version:
            rank = 3 if wild else 4
        elif r[2] == ANY_VERSION:
            rank = 1 if wild else 2
        else:
            continue
        if rank > best_rank:
            best, best_rank = r, rank
    return best, best_rank


def cheats_of(blob, row, blob_off, str_off):
    """Names are offsets into the game's OWN blob, not into a database-wide string table -- format
    2 moved them there so a lookup reads one game's few kilobytes instead of all 769,488."""
    _cc, _code, _v, n, off = row
    at = blob_off + off
    out = []
    for i in range(n):
        name_off, first, cnt = struct.unpack(">IHH", blob[at + i * GROUP_ROW:at + (i + 1) * GROUP_ROW])
        end = blob.index(b"\0", at + name_off)
        name = blob[at + name_off:end].decode("utf-8", "replace")
        codes_at = at + n * GROUP_ROW + first * CODE_ROW
        lines = [struct.unpack(">II", blob[codes_at + j * CODE_ROW:codes_at + (j + 1) * CODE_ROW])
                 for j in range(cnt)]
        out.append((name, lines))
    return out


def rom_key(path):
    """(game_code, version) out of a ROM header, byte order fixed up."""
    raw = open(path, "rb").read(0x40)
    if raw[:4] == b"\x37\x80\x40\x12":                       # .v64, byte-swapped
        raw = bytes(b for p in zip(raw[1::2], raw[0::2]) for b in p)
    elif raw[:4] == b"\x40\x12\x37\x80":                     # .n64, little-endian
        raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
    elif raw[:4] != b"\x80\x37\x12\x40":
        return None, None, None
    return raw[0x3B:0x3F].decode("ascii", "replace"), raw[0x3F], raw[0x20:0x34].decode(
        "ascii", "replace").rstrip("\0 ")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("db")
    ap.add_argument("code", nargs="?", help="four-character game code, e.g. NEPE")
    ap.add_argument("-v", "--version", type=int, default=ANY_VERSION)
    ap.add_argument("--rom", nargs="+", help="read the key out of these ROMs instead")
    ap.add_argument("--grep", help="only cheats whose name contains this")
    ap.add_argument("--list", action="store_true", help="print every cheat, not just a count")
    args = ap.parse_args()

    blob, rows, blob_off, str_off = load(args.db)
    RANK = {5: "check code", 4: "exact code + version", 3: "wildcard + version",
            2: "exact code", 1: "wildcard", 0: "-"}

    targets = []
    if args.rom:
        for pat in args.rom:
            for p in sorted(glob.glob(pat)) or [pat]:
                code, ver, title = rom_key(p)
                if code is None:
                    print("%-38s not a ROM" % os.path.basename(p)[:37])
                    continue
                targets.append((os.path.basename(p), code, ver, title))
    else:
        if not args.code:
            ap.error("give a game code, or --rom")
        targets.append((args.code, args.code, args.version, ""))

    for label, code, ver, title in targets:
        row, rank = find_row(rows, 0, code, ver)
        if row is None:
            print("%-38s %s v%-3d  NO ROW" % (label[:37], code, ver))
            continue
        got = cheats_of(blob, row, blob_off, str_off)
        shown = [c for c in got if not args.grep or args.grep.lower() in c[0].lower()]
        print("%-38s %s v%-3d -> %s v%-3d  %4d cheats   (%s)"
              % (label[:37], code, ver, row[1], row[2], len(got), RANK[rank]))
        if args.list or args.grep:
            for name, lines in shown:
                print("      %-44s %s" % (name[:43],
                      " + ".join("%08X %04X" % (a, v) for a, v in lines[:3])
                      + (" +%d more" % (len(lines) - 3) if len(lines) > 3 else "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
