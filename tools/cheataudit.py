#!/usr/bin/env python3
"""
Every group in cheats.db, classified by what the engine can theoretically do.

    tools/cheataudit.py                         # the database alone
    tools/cheataudit.py --shelf ~/roms/n64      # plus hook/fit against a folder of ROMs
    tools/cheataudit.py --tsv build/cheataudit.tsv

The converter already refuses what body_words() cannot emit. This asks the questions the
converter cannot: does the write sit in the 4 MB that every console has, or only in the
Expansion Pak; and, when a ROM is in hand, can that image actually be hooked and can the
group fit in its padding.

A group marked 4mb is a theory that the write lands in RAM the game has. It is not a proof
the cheat does what its name says -- that needs the right binary and a playtest. A group
marked 8mb will do nothing on a console with no Pak, and that is a fact.
"""

import argparse
import glob
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cheatlook as look
import rompatch as rp

try:
    import cheatshelf
except ImportError:
    cheatshelf = None


def group_words(lines):
    """Same fit rule as mkcheatdb.filter_group / cheatshelf.group_words."""
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


def write_off(raw):
    """RDRAM offset the engine will store to. Mirrors emit_body()."""
    return raw & 0x007FFFFF


def classify_lines(lines):
    """Return (kinds, band, tests, writes) for one group.

    band is '4mb', '8mb', 'beyond', or 'none'. 8mb means at least one write is at or above
    4 MB -- the group needs an Expansion Pak or those stores land on nothing.
    """
    kinds, offs, tests, writes = set(), [], 0, 0
    i = 0
    while i < len(lines):
        kind = (lines[i][0] >> 24) & 0xFF
        kinds.add(kind)
        if (kind & 0xF8) == 0xD0:
            tests += 1
            i += 1
            continue
        if kind == 0x50:
            writes += 1
            if i + 1 < len(lines):
                offs.append(write_off(lines[i + 1][0]))
                kinds.add((lines[i + 1][0] >> 24) & 0xFF)
                i += 2
                continue
        if (kind & 0xF0) in (0x80, 0xA0) or kind in (0xF0, 0xF1, 0xEE):
            writes += 1
            # 0xEE writes osMemSize at 0x80000318, which is in the first 4 MB.
            offs.append(0x318 if kind == 0xEE else write_off(lines[i][0]))
        i += 1

    band = "none"
    if offs:
        if any(o >= 0x800000 for o in offs):
            band = "beyond"
        elif any(o >= 0x400000 for o in offs):
            band = "8mb"
        else:
            band = "4mb"
    return kinds, band, tests, writes


def shape(kinds, tests):
    if 0x50 in kinds:
        return "repeater"
    if 0xEE in kinds:
        return "osMemSize"
    if tests:
        return "conditional"
    if kinds & {0xF0, 0xF1}:
        return "boot-write"
    return "write"


def load_groups(db):
    blob, rows, blob_off, str_off = look.load(db)
    out = []
    emit_fail = 0
    for row in rows:
        for name, lines in look.cheats_of(blob, row, blob_off, str_off):
            words = group_words(lines)
            kinds, band, tests, writes = classify_lines(lines)
            if words is None:
                emit_fail += 1
            out.append({
                "code": row[1],
                "ver": row[2],
                "name": name,
                "lines": len(lines),
                "words": words,
                "band": band,
                "shape": shape(kinds, tests),
                "kinds": kinds,
            })
    return blob, rows, blob_off, str_off, out, emit_fail


def summarise(groups, emit_fail):
    n = len(groups)
    by_band = {k: 0 for k in ("4mb", "8mb", "beyond", "none")}
    by_shape = {}
    oversize = 0
    for g in groups:
        by_band[g["band"]] += 1
        by_shape[g["shape"]] = by_shape.get(g["shape"], 0) + 1
        if g["words"] is not None and g["words"] > 128:
            oversize += 1
    return n, by_band, by_shape, oversize, emit_fail


def print_summary(n, by_band, by_shape, oversize, emit_fail, games):
    print("games in database:  %d" % games)
    print("groups in database: %d" % n)
    print()
    print("by RAM the write needs")
    print("  4 MB (every console):     %6d  %5.1f%%" % (by_band["4mb"], 100.0 * by_band["4mb"] / n))
    print("  8 MB (Expansion Pak):     %6d  %5.1f%%" % (by_band["8mb"], 100.0 * by_band["8mb"] / n))
    print("  past 8 MB:                %6d  %5.1f%%" % (by_band["beyond"], 100.0 * by_band["beyond"] / n))
    print("  no store (should be 0):   %6d" % by_band["none"])
    print()
    print("by shape")
    for k in ("write", "conditional", "repeater", "boot-write", "osMemSize"):
        if k in by_shape:
            print("  %-14s %6d  %5.1f%%" % (k, by_shape[k], 100.0 * by_shape[k] / n))
    print()
    print("converter / engine agreement")
    print("  emit refused (a leak):    %6d" % emit_fail)
    print("  over 128 words:           %6d" % oversize)
    print()
    print("theory")
    print("  should work, 8 MB console, if the ROM hooks and the group fits:  %d"
          % (by_band["4mb"] + by_band["8mb"]))
    print("  should work on 4 MB too:                                         %d" % by_band["4mb"])
    print("  will do nothing without an Expansion Pak:                        %d" % by_band["8mb"])


def write_tsv(path, groups):
    with open(path, "w", encoding="utf-8") as f:
        f.write("code\tver\tband\tshape\twords\tlines\tname\n")
        for g in groups:
            f.write("%s\t%s\t%s\t%s\t%s\t%d\t%s\n" % (
                g["code"],
                "any" if g["ver"] == look.ANY_VERSION else str(g["ver"]),
                g["band"], g["shape"],
                "-" if g["words"] is None else str(g["words"]),
                g["lines"], g["name"].replace("\t", " ")))


def shelf_audit(shelf, blob, rows, blob_off, str_off):
    if cheatshelf is None:
        sys.exit("cheatshelf.py failed to import")
    paths = []
    for root, _dirs, files in os.walk(shelf):
        for fn in files:
            if fn.lower().endswith((".z64", ".n64", ".v64")):
                paths.append(os.path.join(root, fn))
    paths.sort()

    hook_ok = hook_none = no_row = crc_fail = 0
    fit_all = fit_some = fit_none = 0
    lines = []
    for p in paths:
        name, r = cheatshelf.audit(p, blob, rows, blob_off, str_off)
        if r is None:
            continue
        code, ver, row, rank, fits, total, words, hook, notes, title = r
        note = "; ".join(notes)
        if row is None:
            no_row += 1
            status = "no-row"
        elif hook == "NONE":
            hook_none += 1
            status = "no-hook"
        elif any("CRC GATE" in n for n in notes):
            crc_fail += 1
            status = "crc-gate"
        else:
            hook_ok += 1
            if total == 0:
                status = "empty-row"
            elif fits == total:
                fit_all += 1
                status = "all-fit"
            elif fits == 0:
                fit_none += 1
                status = "none-fit"
            else:
                fit_some += 1
                status = "some-fit"
        lines.append((status, name, code, ver, fits, total, words, hook, note, title))
    return paths, lines, {
        "roms": len(paths),
        "hook_ok": hook_ok,
        "hook_none": hook_none,
        "no_row": no_row,
        "crc_fail": crc_fail,
        "fit_all": fit_all,
        "fit_some": fit_some,
        "fit_none": fit_none,
    }


def print_shelf(stats, lines):
    print()
    print("shelf (%d ROMs)" % stats["roms"])
    print("  hookable:                 %6d" % stats["hook_ok"])
    print("  no usable preamble:       %6d" % stats["hook_none"])
    print("  CRC gate refuses image:   %6d" % stats["crc_fail"])
    print("  no database row:          %6d" % stats["no_row"])
    print("  of hookable with cheats:")
    print("    every group fits:       %6d" % stats["fit_all"])
    print("    some groups fit:        %6d" % stats["fit_some"])
    print("    nothing fits padding:   %6d" % stats["fit_none"])
    print()
    print("games that cannot run a cheat (hook or CRC)")
    for status, name, code, ver, fits, total, words, hook, note, _t in lines:
        if status in ("no-hook", "crc-gate"):
            print("  %-40s %s v%d  %s  %s"
                  % (name[:40], code, ver, hook, note))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", default="build/cheats.db")
    ap.add_argument("--shelf", help="folder of ROMs to also run the hook/fit audit on")
    ap.add_argument("--tsv", help="write one line per group")
    args = ap.parse_args()

    if not os.path.isfile(args.db):
        sys.exit("%s is missing -- build it with tools/mkcheatdb.py --fetch" % args.db)

    blob, rows, blob_off, str_off, groups, emit_fail = load_groups(args.db)
    n, by_band, by_shape, oversize, emit_fail = summarise(groups, emit_fail)
    print_summary(n, by_band, by_shape, oversize, emit_fail, len(rows))

    if args.tsv:
        write_tsv(args.tsv, groups)
        print()
        print("wrote %s" % args.tsv)

    if args.shelf:
        _paths, lines, stats = shelf_audit(args.shelf, blob, rows, blob_off, str_off)
        print_shelf(stats, lines)

    return 0


if __name__ == "__main__":
    sys.exit(main())
