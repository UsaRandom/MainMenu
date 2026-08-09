#!/usr/bin/env python3
"""
Build sd:/menu/cheats.db from libretro's cheat corpus.

    tools/mkcheatdb.py --fetch -o build/cheats.db     # download the corpus, then convert
    tools/mkcheatdb.py -i build/cht -o build/cheats.db

**Output is a release artifact placed on the SD card. It is never committed and never linked
into the ROM.** The corpus is not committed either.

Corpus: libretro/libretro-database, `cht/Nintendo - Nintendo 64/`. MIT, actively maintained,
several hundred games. Chosen over the two obvious alternatives for reasons worth keeping:
mupen64plus' mupencheat.txt is GPLv2-only and this repo is AGPL-3.0, which is incompatible for a
combined work; Project64's Cheats.rdb has murky provenance. The codes themselves are addresses
and values -- unoriginal facts -- but the descriptions and the selection plausibly are
protectable, so taking an MIT compilation sidesteps the question instead of answering it.

## What gets dropped, and why it has to be

The filter is not a list here. It is `rompatch.body_words()`, imported, which mirrors
`rompatch_body_words()` in src/menu/rompatch_find.c -- the console's own answer to "can this line
be emitted". Anything written into the database that the engine then refuses is a cheat a player
can tick and watch do nothing, on a machine with no diagnosis available, which is worse than one
that is absent.

That used to be a list, and it drifted from the engine in both directions: it wrote 652 repeater
groups and 348 boot-write groups the console silently refused at launch, and it dropped 51 groups
as "GS-button-only" that had nothing to do with the button (0xEE and 0xFF have bit 3 set, but bit
3 only means the button in a write or conditional type byte).

What is left out, measured over the corpus, is now only what genuinely cannot run:

  * every GS-button variant -- 88 89 A8 A9 D8-DB. There is no GameShark button on this console
    and nothing in RDRAM stands in for one, so these can never do anything. 2,520 groups.
  * a conditional or repeater with nothing after it to guard. 16,653 groups -- and almost all of
    them are not cheats: 20,142 of the 20,154 dangling conditionals in the corpus are named
    "Activator", building blocks the corpus lists separately for a player to combine by hand.
  * CC DE FF, which emit nothing even on Datel's own engine. 29 groups.
  * a 16-bit access at an odd address, which would not misbehave but hang: `sh` off an odd
    address takes an Address Error at the exception vector with EXL set and vectors straight back
    in. 531 groups, all of which earlier versions of this converter wrote out.

**A group in which any line is dropped is dropped whole**, and counted. Half a cheat is not a
cheat: a conditional without its write leaves the engine pairing the D0 with an unrelated code.

Coverage is written to build/cheatdb-report.txt so what was lost is visible rather than implied.
"""

import argparse
import io
import os
import re
import struct
import sys
import urllib.error
import urllib.request
import zipfile

MAGIC = 0x4D363443              # 'M64C'
FORMAT_VER = 2
ANY_VERSION = 0xFF

CORPUS_ZIP = "https://github.com/libretro/libretro-database/archive/refs/heads/master.zip"
CORPUS_SUBDIR = "cht/Nintendo - Nintendo 64/"

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import rompatch

# What the runtime engine can and cannot emit is rompatch.body_words(), which is itself a mirror of
# rompatch_body_words() in src/menu/rompatch_find.c. Imported rather than restated: this file used
# to carry its own ACCEPTED set and its own idea of what a conditional may guard, and the two
# drifted -- it counted 51 groups as "GS-button-only" that were nothing of the sort, and it wrote
# 652 repeater groups and 348 boot-write groups into the database that the console then refused at
# launch, silently, with no way for a player to tell why the cheat they ticked did nothing.
ENGINE_MAX_WORDS = 128


def fetch_corpus(dest):
    """Download the corpus zip and extract just the N64 cheat files."""
    os.makedirs(dest, exist_ok=True)
    print("fetching %s ..." % CORPUS_ZIP, file=sys.stderr)
    try:
        with urllib.request.urlopen(CORPUS_ZIP, timeout=300) as r:
            blob = r.read()
    except urllib.error.URLError as e:
        sys.exit("cannot reach GitHub: %s" % e)

    n = 0
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        # Record what we actually took, because corpora get relicensed and a bare "MIT" in a
        # comment ages badly without a commit to point at.
        names = z.namelist()
        root = names[0].split("/")[0]
        for name in names:
            if CORPUS_SUBDIR not in name or not name.endswith(".cht"):
                continue
            out = os.path.join(dest, os.path.basename(name))
            with z.open(name) as src, open(out, "wb") as dst:
                dst.write(src.read())
            n += 1
        lic = [x for x in names if x.endswith("/LICENSE") and x.count("/") == 2]
        if lic:
            with z.open(lic[0]) as src:
                head = src.read(400).decode("utf-8", "replace")
            with open(os.path.join(dest, "CORPUS-LICENSE.txt"), "w") as f:
                f.write("from %s\n\n%s" % (root, head))
    print("%d .cht files -> %s" % (n, dest), file=sys.stderr)
    return n


def parse_cht(path):
    """libretro .cht -> [(name, [(addr, val), ...]), ...]."""
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    desc = dict(re.findall(r'cheat(\d+)_desc\s*=\s*"([^"]*)"', text))
    code = dict(re.findall(r'cheat(\d+)_code\s*=\s*"([^"]*)"', text))

    groups = []
    for k in sorted(code, key=int):
        lines = []
        ok = True
        # Codes are "AAAAAAAA VVVV" pairs joined by '+'.
        for tok in code[k].split("+"):
            tok = tok.strip()
            m = re.match(r'^([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{4})$', tok)
            if not m:
                ok = False
                break
            lines.append((int(m.group(1), 16), int(m.group(2), 16)))
        if ok and lines:
            groups.append((desc.get(k, "Cheat %s" % k), lines))
    return groups


def why_refused(lines, i, tests):
    """A reason a reader of the report can act on, for the block starting at lines[i]."""
    t = (lines[i][0] >> 24) & 0xFF
    j = i + tests
    if j >= len(lines):
        return "type %02X with nothing after it to guard" % t
    bt = (lines[j][0] >> 24) & 0xFF
    # The specials are matched before the bit-field reading, exactly as the switch in
    # src/boot/cheats.c does. 0xDE is SPECIAL_SET_ENTRYPOINT_ADDR, not "a 16-bit not-equal
    # conditional behind the GS button" -- reading its bits was how 15 groups came to be reported
    # as GS-button-only when the button has nothing to do with them.
    if bt in (0xCC, 0xDE, 0xFF, 0x20, 0x00):
        return "type %02X emits nothing even on Datel's own engine" % bt
    if (bt & 0x08) and (bt & 0xF0) in (0x80, 0xA0, 0xD0):
        return "type %02X only acts while the GS button is held, and there is no button" % bt
    if rompatch.halfword_misaligned(lines[j][0]):
        return "type %02X is 16-bit at the odd address %08X, which would hang the console" \
               % (bt, lines[j][0] & 0x00FFFFFF)
    if bt == 0x50:
        if (lines[j][0] >> 8) & 0xFF == 0:
            return "repeater with a count of zero"
        if j + 1 >= len(lines):
            return "repeater with no write after it to multiply"
        nt = (lines[j + 1][0] >> 24) & 0xFF
        if rompatch.halfword_misaligned(lines[j + 1][0]):
            return "repeater over a 16-bit write at an odd address, which would hang the console"
        if (nt & 0x01) and ((lines[j][0] & 0xFF) & 1):
            return "repeater stepping a 16-bit write by an odd %d bytes" % (lines[j][0] & 0xFF)
        return "repeater over type %02X, which is not a write" % nt
    if tests:
        return "type %02X guards type %02X, which is not something the engine can emit" % (t, bt)
    return "type %02X is not something the engine can emit" % bt


def filter_group(lines):
    """Return (kept_lines, reason) -- reason is None when the whole group is usable.

    The question this asks is exactly the one the console asks at launch, because it asks it with
    the console's own code: rompatch.body_words() is a line-for-line mirror of
    rompatch_body_words() in src/menu/rompatch_find.c. A group written here that the engine then
    refuses is a cheat a player can tick and watch do nothing, with no diagnosis available on the
    hardware -- so the two filters agreeing is not tidiness, it is the only way the database can
    mean what it says.
    """
    words, i = 4, 0
    while i < len(lines):
        tests = 0
        while i + tests < len(lines) and ((lines[i + tests][0] >> 24) & 0xF8) == 0xD0 \
                and not rompatch.halfword_misaligned(lines[i + tests][0]):
            tests += 1
        body, eaten = rompatch.body_words(lines, i + tests)
        if body == 0:
            return None, why_refused(lines, i, tests)
        words += 4 * tests + body
        i += tests + eaten

    # The engine's own ceiling, checked here so the report says so once rather than the console
    # discovering it per launch. It is the looser of the two limits: how much dead padding the game
    # has decides most of them, and that is a per-ROM number no converter can know.
    if words > ENGINE_MAX_WORDS:
        return None, "%d engine words, over the %d the engine holds" % (words, ENGINE_MAX_WORDS)
    return lines, None


def load_keys(path):
    """filename -> (game_code, version, check_code, region_byte), from a table of pure facts.

    The region byte is the fifth column and may be empty. It is not part of the key; it is what
    the key would be *narrowed to* if the cartridge's header agrees. See region_rows().
    """
    keys = {}
    if not path or not os.path.exists(path):
        return keys
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) < 4:
            continue
        keys[parts[0]] = (parts[1], int(parts[2]), int(parts[3], 16),
                          parts[4].strip() if len(parts) > 4 else "")
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", default="build/cht", help="directory of .cht files")
    ap.add_argument("-o", "--output", default="build/cheats.db")
    ap.add_argument("--fetch", action="store_true", help="download the corpus first")
    ap.add_argument("--keys", default="tools/data/n64_keys.tsv",
                    help="filename -> game_code/version/check_code table")
    ap.add_argument("--report", default="build/cheatdb-report.txt")
    args = ap.parse_args()

    if args.fetch:
        fetch_corpus(args.input)

    if not os.path.isdir(args.input):
        sys.exit("no corpus at %s -- run with --fetch" % args.input)

    keys = load_keys(args.keys)
    if not keys:
        print("warning: no key table at %s; every game will be keyed by filename guess only"
              % args.keys, file=sys.stderr)

    report = []
    games = []
    dropped_total = 0
    kept_total = 0

    for fn in sorted(os.listdir(args.input)):
        if not fn.endswith(".cht"):
            continue
        stem = fn[:-4]
        groups = parse_cht(os.path.join(args.input, fn))
        if not groups:
            continue

        kept, drops = [], []
        for name, lines in groups:
            good, reason = filter_group(lines)
            if good is None:
                drops.append((name, reason))
            else:
                kept.append((name, good))

        report.append("%s: %d kept, %d dropped" % (stem, len(kept), len(drops)))
        for name, reason in drops:
            report.append("    DROP %-44s %s" % (name[:44], reason))

        if not kept:
            continue

        # Counted only once the game is actually written. Counting here originally included
        # games that were dropped for want of a key two lines later, so the report claimed
        # 199,675 cheats when a third of them were in games nothing could ever look up.
        game_code, version, check_code, region = keys.get(stem, (None, ANY_VERSION, 0, ""))
        if game_code is None:
            # No key: the entry is still emitted, keyed by check_code 0, so it can never match.
            # Recorded rather than silently skipped, because "350 games converted" meaning "40 of
            # them reachable" is exactly the kind of number that misleads.
            report.append("    NOKEY %s -- not reachable without a key table row" % stem)
            continue

        # code_first and cheat_count are uint16 on disk, so a game with more than 65,535 code
        # lines would wrap the offset and hand the engine addresses from the wrong cheat. The
        # corpus's largest entries run to five figures, so this is reachable, not theoretical.
        total_lines = sum(len(l) for _n, l in kept)
        if total_lines > 0xFFFF or len(kept) > 0xFFFF:
            keep_n, running = 0, 0
            for _n, l in kept:
                if running + len(l) > 0xFFFF or keep_n >= 0xFFFF:
                    break
                running += len(l)
                keep_n += 1
            report.append("    TRUNC %s: %d of %d cheats kept, uint16 line offset would wrap"
                          % (stem, keep_n, len(kept)))
            kept = kept[:keep_n]
            if not kept:
                continue

        dropped_total += len(drops)
        kept_total += len(kept)
        games.append((check_code, game_code, version, region, kept))

    if not games:
        sys.exit("no games could be keyed; supply --keys (see tools/data/n64_keys.tsv)")

    # Merge rows that share a key. The corpus carries several files per game -- regional
    # variants, revisions, differently-sourced dumps -- and the key table maps them all to one
    # header key, so without this the index held five NAG? rows and the reader's linear scan
    # returned whichever came first: measured, a 3-cheat row winning over a 142-cheat one.
    #
    # Merged rather than "keep the biggest", because the variants genuinely differ; deduplicated
    # by name so the same cheat from three files appears once.
    #
    # ## And then narrowed again, per region
    #
    # Merging every region into one row is what made "Infinite Money" on a USA Star Wars Racer
    # write to 0x8111CB18, which is where the truguts live in the *European* binary, 0x8CA0 away
    # from where they live in this one. It is not a rare shape: 183 of the 394 keys are fed by more
    # than one region, and because the dedup keeps the first file alphabetically -- and "(E)",
    # "(Europe)" and "(F)" all sort before "(USA)" -- 11,162 USA cheats across 136 games were being
    # replaced outright by a same-named foreign one. Tetrisphere lost 7,183 of them.
    #
    # So every file whose name states a region also feeds a second, narrower row whose fourth
    # character is the byte the header will actually carry. find_row() in cheatdb.c ranks an exact
    # four-character code above the three-character wildcard, so a cartridge takes the narrow row
    # when there is one -- and the wildcard row is left exactly as it was, holding everything
    # merged, so a release whose region byte the filename did not predict is no worse off than
    # before. Never worse, usually right; the cost is that a mixed game's cheats are stored twice.
    merged = {}
    narrowed = 0
    for check_code, game_code, version, region, kept in games:
        want = [(check_code, game_code, version)]
        if region and game_code.endswith("?"):
            want.append((check_code, game_code[:3] + region, version))
            narrowed += 1
        for k in want:
            if k not in merged:
                merged[k] = ([], set())
            rows, seen = merged[k]
            for name, lines in kept:
                if name in seen:
                    continue
                seen.add(name)
                rows.append((name, lines))

    # A narrow row that came out identical to its wildcard parent is pure duplication: the game had
    # only one region's files, so there was nothing to separate. Dropping those is worth 1.1 MB of
    # the 2.2 MB the split would otherwise cost, and it costs nothing at all -- the cartridge falls
    # through to the wildcard row and finds the same cheats in the same order.
    identical = 0
    for (cc, gc, v) in list(merged):
        if gc.endswith("?"):
            continue
        parent = merged.get((cc, gc[:3] + "?", v))
        if parent is not None and parent[1] == merged[(cc, gc, v)][1]:
            del merged[(cc, gc, v)]
            identical += 1
    if identical:
        report.append("dropped %d region rows that matched their wildcard row exactly" % identical)

    before = len(games)
    games = [(cc, gc, v, rows) for (cc, gc, v), (rows, _s) in merged.items()]
    if before != len(games):
        report.append("")
        report.append("merged %d corpus entries into %d unique header keys, of which %d are "
                      "region-specific rows beside a wildcard one"
                      % (before, len(games), narrowed))

    kept_total = sum(len(rows) for _c, _g, _v, rows in games)

    games.sort(key=lambda g: g[0])          # sorted by check_code, so the reader can bisect

    # --- serialise -------------------------------------------------------------------
    #
    # Every game's blob is self-contained: group rows, then code lines, then the names those rows
    # point at, all contiguous, with the total length in the index. So a load is one seek and one
    # read of a few kilobytes.
    #
    # Format 1 kept a single string table for the whole database, and the reader had to read the
    # WHOLE of it to resolve one game's names -- 769,488 bytes on the shipped file, on every
    # detail sheet that opened. That is far longer than the audio the mixer holds ahead of the
    # DAC, so the music stopped dead each time. Interning across games saved 41 KB of card space
    # and cost three quarters of a megabyte of I/O per lookup.
    blob = bytearray()
    index = bytearray()

    for check_code, game_code, version, kept in games:
        off = len(blob)

        # Names sit after the group rows and the codes, so their offsets are known in advance
        # from the two counts -- no second pass and no patching up.
        total_lines = sum(len(l) for _n, l in kept)
        names_base = len(kept) * 8 + total_lines * 8
        names = bytearray()
        seen = {}

        def local_intern(s, names=names, seen=seen, names_base=names_base):
            b = s.encode("utf-8", "replace")[:127]
            if b in seen:
                return seen[b]
            off_ = names_base + len(names)
            seen[b] = off_
            names.extend(b)
            names.append(0)
            return off_

        code_rows = []
        grp_rows = bytearray()
        first = 0
        for name, lines in kept:
            grp_rows += struct.pack(">IHH", local_intern(name), first, len(lines))
            code_rows.extend(lines)
            first += len(lines)

        game = bytearray(grp_rows)
        for addr, val in code_rows:
            game += struct.pack(">II", addr, val)
        game += names
        blob += game

        index += struct.pack(">Q4sBBHII", check_code,
                             game_code.encode("ascii")[:4].ljust(4, b"\0"),
                             version, 0, len(kept), off, len(game))

    # Kept as an empty region so the header keeps its shape. Nothing reads it any more.
    strtab = bytearray(b"\0")

    header_size = 64
    index_off = header_size
    blob_off = index_off + len(index)
    strtab_off = blob_off + len(blob)

    header = struct.pack(">IHHIIIIII", MAGIC, FORMAT_VER, 0, len(games),
                         index_off, blob_off, strtab_off, len(strtab), 0)
    header += b"\0" * (header_size - len(header))

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(header)
        f.write(index)
        f.write(blob)
        f.write(strtab)

    os.makedirs(os.path.dirname(os.path.abspath(args.report)), exist_ok=True)
    with open(args.report, "w") as f:
        f.write("games in corpus: %d\ngames written:   %d\n"
                "cheats kept:     %d\ncheats dropped:  %d\n\n"
                % (len(os.listdir(args.input)), len(games), kept_total, dropped_total))
        f.write("\n".join(report))

    print("%d games, %d cheats, %d dropped -> %s (%.1f KB)"
          % (len(games), kept_total, dropped_total, args.output,
             os.path.getsize(args.output) / 1024.0))
    print("coverage report: %s" % args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
