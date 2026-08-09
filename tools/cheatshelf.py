#!/usr/bin/env python3
"""
Per-ROM cheat audit: what each game on a shelf is offered, and whether the engine can deliver it.

    tools/cheatshelf.py ~/roms/n64/*.z64
    tools/cheatshelf.py --db build/cheats.db --verbose ~/roms/n64/*.z64

## Why this exists

Everything about cheats that has gone wrong has gone wrong *per game*, and silently. Ocarina V1.2
was handed V1.0's addresses. A USA Star Wars Racer was offered the European "Infinite Money".
Mario Party 3's preamble scan picked a dispatcher stub instead of the real one. GoldenEye has no
usable preamble at all and never could run a cheat. In every case the console did exactly what it
was told, the television showed nothing, and the only way to find out was to own the game and try.

So: read each ROM's header, do the database lookup the console does, run the real preamble scan and
the real padding scan over the real image, and print one line per game. Nothing here is
reimplemented -- it imports cheatlook (which transcribes find_row), preamblescan and rompatch.

Columns:

    code/ver  the header key, and how the row was reached: `code+v` beat `wild`
    cheats    how many of the row's cheats this ROM's own padding could actually hold, of the total
    words     usable padding words in the boot segment, guards already deducted
    hook      where the engine would attach, or why it cannot
"""

import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import importlib.util


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


HERE = os.path.dirname(os.path.abspath(__file__))
look = _load("cheatlook", os.path.join(HERE, "cheatlook.py"))
rp = _load("rompatch", os.path.join(HERE, "rompatch.py"))
import preamblescan
import romcrc

RANK = {5: "cc", 4: "code+v", 3: "wild+v", 2: "code", 1: "wild", 0: "-"}


def group_words(lines):
    """Engine words this group needs, or None if it cannot be emitted. Mirrors the fit rule."""
    w, i = 4, 0
    while i < len(lines):
        t = 0
        while i + t < len(lines) and ((lines[i + t][0] >> 24) & 0xF8) == 0xD0 \
                and not rp.halfword_misaligned(lines[i + t][0]):
            t += 1
        bw, eaten = rp.body_words(lines, i + t)
        if bw == 0:
            return None
        w += 4 * t + bw
        i += t + eaten
    return w


def audit(path, blob, rows, blob_off, str_off):
    name = os.path.basename(path)
    code, ver, title = look.rom_key(path)
    if code is None:
        return name, None

    row, rank = look.find_row(rows, 0, code, ver)
    notes, words, hook, fits, total = [], 0, "-", 0, 0

    img = preamblescan.normalise(open(path, "rb").read(rp.WINDOW_OFF + rp.WINDOW_LEN))
    if img is None:
        notes.append("not a recognisable image")
    else:
        try:
            cic, _hdr, entry = rp.identify(img)
        except SystemExit as e:
            cic = None
            notes.append(str(e).split(":")[0])
        if cic:
            h1, h2 = struct.unpack_from(">II", img, 0x10)
            if (h1, h2) != romcrc.crc(img, cic):
                notes.append("CRC GATE FAILS -- the engine refuses to touch this image")
            words = sum(w for _a, w in rp.usable_runs(img))
            if words == 0:
                notes.append("no padding: nothing can be placed")
            win = preamblescan.best(img, entry)
            if win is None:
                cand = len(preamblescan.scan(img))
                hook = "NONE"
                notes.append("no usable preamble (%d candidate%s)"
                             % (cand, "" if cand == 1 else "s"))
            else:
                hook = "%08x" % (entry + (win[0] - rp.WINDOW_OFF))
                if win[4] < 2:
                    notes.append("preamble %+d away%s"
                                 % (win[3] - (entry + (win[0] - rp.WINDOW_OFF)),
                                    "" if win[4] == 1 else ", target is NOT __osException"))

    if row is not None:
        cheats = look.cheats_of(blob, row, blob_off, str_off)
        total = len(cheats)
        budget = min(128, max(0, words - 2))
        for _cname, lines in cheats:
            w = group_words(lines)
            if w is not None and w <= budget:
                fits += 1
    return name, (code, ver, row, rank, fits, total, words, hook, notes, title)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("rom", nargs="+")
    ap.add_argument("--db", default="build/cheats.db")
    ap.add_argument("--verbose", action="store_true", help="also list the cheats that do not fit")
    args = ap.parse_args()

    blob, rows, blob_off, str_off = look.load(args.db)

    paths = []
    for pat in args.rom:
        paths.extend(sorted(glob.glob(pat)) or [pat])

    print("%-34s %-5s %-4s %-6s %-7s %-10s %-6s %s"
          % ("rom", "code", "ver", "row", "how", "cheats", "words", "hook / why not"))
    bad = 0
    for p in paths:
        name, r = audit(p, blob, rows, blob_off, str_off)
        if r is None:
            print("%-34s not a ROM" % name[:33])
            continue
        code, ver, row, rank, fits, total, words, hook, notes, _t = r
        if notes or row is None:
            bad += 1
        print("%-34s %-5s %-4d %-6s %-7s %-10s %-6d %s"
              % (name[:33], code, ver, row[1] if row else "-", RANK[rank],
                 "%d/%d" % (fits, total) if row else "none", words,
                 hook if not notes else hook + "  " + "; ".join(notes)))
    print()
    print("  %d of %d games have something wrong with them" % (bad, len(paths)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
