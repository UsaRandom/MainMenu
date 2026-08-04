#!/usr/bin/env python3
"""
Build tools/data/n64_keys.tsv: libretro cheat filename -> game_code, version, check_code.

    tools/mkcheatkeys.py -i build/cht -o tools/data/n64_keys.tsv

libretro's cheat files are keyed by No-Intro ROM filename. The menu knows a game by its header:
game code, version, check code. Nothing bridges the two, so this builds the bridge from the one
table we already have -- the 440 MATCH_* rows in src/menu/rom_info.c, which is ares' database and
carries a human title in a trailing comment on every row.

The join is on normalised titles. That is a heuristic and it is wrong sometimes, so:

  * matching is exact-after-normalisation, never fuzzy. "Close enough" here means a cheat for one
    game silently applied to another, which is worse than no cheat -- the codes write to
    arbitrary addresses in a game that never expected them.
  * every unmatched corpus entry is listed in the report rather than dropped quietly.
  * the output is a table of pure facts (a filename and three header fields), which is why it is
    the one part of this pipeline that IS committed.

Normalisation drops region and language parentheticals, articles, punctuation and case, because
"007 - The World Is Not Enough (USA)" and "007 The World is Not Enough" are the same game and
differ in all four.
"""

import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPOROOT = os.path.dirname(HERE)

# Parentheticals that describe the release rather than the game.
DROP_PAREN = re.compile(
    r"\((?:USA|Europe|Japan|Australia|Germany|France|Spain|Italy|Korea|China|World|"
    r"U|E|J|A|G|F|S|I|K|Beta|Proto|Demo|Rev [A-Z0-9]|V?1\.\d|En|Fr|De|Es|It|Ja|"
    r"[A-Za-z]{1,2}(?:,[A-Za-z]{1,2})+)\)", re.I)


def normalise(s):
    s = s.strip()
    s = DROP_PAREN.sub(" ", s)
    s = re.sub(r"\([^)]*\)", " ", s)          # any remaining parenthetical
    s = re.sub(r"\[[^\]]*\]", " ", s)
    s = s.lower()
    s = s.replace("&", " and ")
    s = re.sub(r"\b(the|a|an)\b", " ", s)
    s = re.sub(r"[^a-z0-9]+", "", s)
    return s


def harvest_database(path):
    """[(game_code, version, check_code, title)] from the MATCH_* rows."""
    src = open(path, encoding="utf-8").read()
    out = []

    id_pat = re.compile(
        r'MATCH_(ID|ID_REGION|ID_REGION_VERSION)\s*\(\s*"([A-Z0-9]{3,4})"'
        r'(?:\s*,\s*(\d+))?[^)]*\)\s*,\s*//\s*(.+)')
    for kind, code, ver, title in (m.groups() for m in id_pat.finditer(src)):
        if kind == "ID" and len(code) == 3:
            # MATCH_ID matches ANY region, so the fourth character is a wildcard rather than a
            # value. Appending "E" here would key a Japanese release to a USA code and the
            # reader would never match it -- NHFJ against a stored NHFE.
            code += "?"
        out.append((code, int(ver) if ver else 0, 0, title.strip()))

    cc_pat = re.compile(r'MATCH_CHECK_CODE\s*\(\s*(0x[0-9A-Fa-f]+)[^)]*\)\s*,\s*//\s*(.+)')
    for cc, title in (m.groups() for m in cc_pat.finditer(src)):
        out.append(("", 0, int(cc, 16), title.strip()))

    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", default="build/cht")
    ap.add_argument("-o", "--output", default=os.path.join(REPOROOT, "tools/data/n64_keys.tsv"))
    ap.add_argument("--rom-info", default=os.path.join(REPOROOT, "src/menu/rom_info.c"))
    ap.add_argument("--report", default="build/cheatkeys-report.txt")
    args = ap.parse_args()

    db = harvest_database(args.rom_info)
    if not db:
        sys.exit("harvested no rows from %s -- did the MATCH_ macros change shape?" % args.rom_info)

    # Group by normalised title first, so a collision can be inspected rather than only counted.
    grouped = {}
    for code, ver, cc, title in db:
        k = normalise(title)
        if not k:
            continue
        grouped.setdefault(k, []).append((code, ver, cc, title))

    by_title = {}
    collisions = set()
    for k, rows_for in grouped.items():
        distinct = {r[:3] for r in rows_for}
        if len(distinct) == 1:
            by_title[k] = rows_for[0]
            continue

        # More than one row wants this title. Some collisions are not real: rom_info carries
        # MATCH_CHECK_CODE rows for individually-dumped oddities -- "Donkey kong 64 [USA CRACK]"
        # and "[PAL CRACK]" sit beside the ordinary MATCH_ID("NDO") row, and the normaliser drops
        # the bracketed suffix that told them apart. The cheat corpus is keyed by No-Intro names
        # for clean dumps, so a cracked-dump row is never the right answer for one of them. When
        # exactly one game-code row is in the group, it is unambiguously the one meant.
        #
        # This does NOT resolve two game codes differing only by region -- Ocarina of Time's CZL
        # and NZL, Mario Party's CLB and NLB. Those are genuinely different games to the cheat
        # engine, addresses and all, and picking either would be the "worse than no cheat" case
        # this file exists to avoid. They stay refused.
        id_rows = {r[:3] for r in rows_for if r[0]}
        if len(id_rows) == 1:
            by_title[k] = next(r for r in rows_for if r[0])
            continue

        collisions.add(k)                     # ambiguous: refuse rather than pick

    rows, misses = [], []
    for fn in sorted(os.listdir(args.input)):
        if not fn.endswith(".cht"):
            continue
        stem = fn[:-4]
        k = normalise(stem)
        if k in collisions:
            misses.append("%s  (ambiguous title, refused)" % stem)
            continue
        hit = by_title.get(k)
        if hit is None:
            misses.append(stem)
            continue
        code, ver, cc, title = hit
        rows.append((stem, code or "????", ver, cc))

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("# libretro cheat filename -> N64 header key.\n"
                "# Generated by tools/mkcheatkeys.py from the MATCH_* rows in src/menu/rom_info.c.\n"
                "# Pure facts: a filename and three header fields. Regenerate rather than edit.\n"
                "# filename\tgame_code\tversion\tcheck_code\n")
        for stem, code, ver, cc in rows:
            f.write("%s\t%s\t%d\t0x%016X\n" % (stem, code, ver, cc))

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w", encoding="utf-8") as f:
        f.write("database rows:   %d\ncorpus files:    %d\nmatched:         %d\nunmatched:       %d\n\n"
                % (len(db), len(rows) + len(misses), len(rows), len(misses)))
        f.write("unmatched corpus entries (no cheats will be reachable for these):\n")
        f.write("\n".join("  " + m for m in misses))

    print("%d of %d corpus files keyed -> %s" % (len(rows), len(rows) + len(misses), args.output))
    print("unmatched: %d (see %s)" % (len(misses), args.report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
