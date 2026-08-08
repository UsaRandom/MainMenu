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


ANY_VERSION_OUT = 0xFF

# Region markers a rom_info comment or a corpus filename uses to tell two releases apart.
# Collapsed to three families, because that is the granularity at which cheat addresses actually
# differ: an NTSC build and a PAL build of the same game are different binaries, a French and a
# German PAL build usually are not.
REGION_FAMILY = {
    "u": "N", "usa": "N", "us": "N", "ntsc": "N", "america": "N",
    "e": "P", "eur": "P", "europe": "P", "pal": "P", "a": "P", "australia": "P",
    "g": "P", "germany": "P", "f": "P", "france": "P", "s": "P", "spain": "P",
    "i": "P", "italy": "P", "uk": "P",
    "j": "J", "jp": "J", "japan": "J",
}


# "(V1.2)", "(Rev 1)" -- the corpus's two ways of naming a revision. Lettered ones ("Rev A") are
# deliberately not decoded: the mapping to a header version byte is a convention rather than a
# fact, and a wrong one here is silently wrong cheats, which is the failure this exists to end.
REVISION = re.compile(r"\(\s*(?:V\s*1\.(\d)|Rev\s+(\d+))\s*\)", re.I)


def revision_of(stem):
    """The ROM header's version byte, if the corpus filename states one, else None.

    This is the fix for AUDIT 2aa. rom_info's Ocarina row is a `MATCH_ID`, which matches any
    region and any revision on purpose -- that is the right granularity for picking a save type
    and the wrong one for picking cheat addresses. Every USA revision therefore keyed to
    `CZL? / ANY`, mkcheatdb merged the three of them and deduplicated by name keeping the first,
    and a V1.2 cartridge was handed V1.0's addresses: Max Heart Containers at 0x8011A5FE instead
    of 0x8011ACAE. The engine wrote it faithfully and nothing happened.

    The filename said "(U) (V1.2)" the whole time. Returning None rather than guessing 0 for an
    unmarked file is what keeps the ANY-version row alive as a fallback -- see find_row() in
    cheatdb.c, which takes an exact version match first and ANY only if there is no better.
    """
    m = REVISION.search(stem)
    if m is None:
        return None
    return int(m.group(1) if m.group(1) is not None else m.group(2))


def region_of(text):
    """The release's region family from its parenthesised markers, or "" if it says nothing.

    Read BEFORE normalisation, which throws these away on purpose -- normalisation exists to make
    "(USA)" and "(Europe)" the same title, and this exists to remember that they are not the same
    ROM. Bracketed alternate titles are dropped first: Ocarina of Time's rom_info comment carries
    "[Zelda no Densetsu - Toki no Ocarina (J)]", and reading the (J) out of the Japanese *title*
    would tag the NTSC row as Japan-only.
    """
    text = re.sub(r"\[[^\]]*\]", " ", text)
    for tok in re.findall(r"\(([^)]*)\)", text):
        for part in re.split(r"[,\s]+", tok):
            fam = REGION_FAMILY.get(part.strip().lower())
            if fam:
                return fam
    return ""


def harvest_database(path):
    """[(game_code, version, check_code, title, region)] from the MATCH_* rows."""
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
        # Only MATCH_ID_REGION_VERSION names a version. The other two match every revision of the
        # game, and writing 0 for them said the opposite: the reader takes an exact version match
        # or the ANY sentinel, so a stored 0 matched revision 0 and nothing else. Five of the
        # twenty-four titles on the test card missed for this reason alone -- Banjo-Kazooie (v1),
        # Star Fox 64 (v1), Rogue Squadron (v1), Pokemon Stadium (v2), Shadows of the Empire (v2)
        # -- each with its game code sitting in the database, holding hundreds of cheats, keyed
        # to a revision nobody owns.
        version = int(ver) if (kind == "ID_REGION_VERSION" and ver) else ANY_VERSION_OUT
        out.append((code, version, 0, title.strip(), region_of(title)))

    cc_pat = re.compile(r'MATCH_CHECK_CODE\s*\(\s*(0x[0-9A-Fa-f]+)[^)]*\)\s*,\s*//\s*(.+)')
    for cc, title in (m.groups() for m in cc_pat.finditer(src)):
        out.append(("", ANY_VERSION_OUT, int(cc, 16), title.strip(), region_of(title)))

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
    for code, ver, cc, title, reg in db:
        k = normalise(title)
        if not k:
            continue
        grouped.setdefault(k, []).append((code, ver, cc, title, reg))

    by_title = {}       # k -> row, for titles only one database row wants
    by_region = {}      # k -> {region family: row}, for titles several rows want
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
        id_rows = {r[:3] for r in rows_for if r[0]}
        if len(id_rows) == 1:
            by_title[k] = next(r for r in rows_for if r[0])
            continue

        # Two game codes for one title is almost always one release per region -- Ocarina of
        # Time's CZL and NZL, Mario Party's CLB and NLB. These used to be refused outright, on the
        # correct reasoning that guessing between them applies one region's addresses to the
        # other's binary, which is worse than having no cheat at all.
        #
        # It is not a guess, though: the corpus filename says which region it is, in the same
        # "(USA)" / "(Europe)" notation the rom_info comments use. So the join gains a second
        # component instead of giving up. It cost the two biggest games on the test card, and it
        # is refused still whenever the filename does not say.
        codes_by_region = {}
        ambiguous = False
        for r in rows_for:
            if not r[0]:
                continue                      # check-code rows carry no game code to key on
            if codes_by_region.setdefault(r[4], r)[:3] != r[:3]:
                ambiguous = True              # two codes claim the same region: still a guess
        if not ambiguous and len(codes_by_region) > 1:
            by_region[k] = codes_by_region
            continue

        collisions.add(k)                     # genuinely ambiguous: refuse rather than pick

    def pick_region(candidates, stem):
        """The row whose region matches @p stem's, or None if the filename does not settle it."""
        fam = region_of(stem)
        if fam in candidates:
            return candidates[fam]
        # An unmarked rom_info row is the NTSC/Japanese release -- the PAL one is what gets the
        # "(E)" or "(PAL)" note, never the other way round in this database.
        if fam in ("N", "J", "") and "" in candidates:
            return candidates[""]
        if fam == "" and "N" in candidates:
            return candidates["N"]
        return None

    rows, misses, revised = [], [], 0
    for fn in sorted(os.listdir(args.input)):
        if not fn.endswith(".cht"):
            continue
        stem = fn[:-4]
        k = normalise(stem)
        if k in collisions:
            misses.append("%s  (ambiguous title, refused)" % stem)
            continue
        hit = by_title.get(k)
        if hit is None and k in by_region:
            hit = pick_region(by_region[k], stem)
            if hit is None:
                misses.append("%s  (title exists per region, filename does not say which)" % stem)
                continue
        if hit is None:
            misses.append(stem)
            continue
        code, ver, cc, title, reg = hit
        # The filename wins over the database row's version, in both directions. rom_info names a
        # revision only when the save type depends on it, so it usually says ANY; and when it does
        # name one, it names the revision whose save type it describes, not the revision these
        # cheat addresses were written against. The corpus filename is the one that knows.
        rev = revision_of(stem)
        if rev is not None:
            ver = rev
            revised += 1
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
    print("%d of those carry a revision from the filename rather than the ANY sentinel" % revised)
    print("unmatched: %d (see %s)" % (len(misses), args.report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
