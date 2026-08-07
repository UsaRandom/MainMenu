#!/usr/bin/env python3
"""
Build a full card of invented games whose cover art is DELIBERATELY the wrong shape some of
the time, so the question "can the tile shape be read off the art?" has something to be
measured against.

This is the third tree, and the three do not overlap:

  mkfixture  real game codes harvested out of rom_info.c, flat gradient art with the code
             stamped on it. For finding bugs. Small: it runs in the regression path.
  mkdemo     invented games, original box art, every cover the right shape. For the README.
  mksample   invented games, original box art, covers at a REALISTIC SPREAD of shapes, and
             enough of them that every tab is a full grid. For looking at layout.

The art is mkdemo's -- same scenes, same palettes, same font -- so a sample card looks like a
card somebody would own rather than like a test pattern. What differs is the aspect each cover
is drawn at, and the number of them.

## Why the shapes are wrong on purpose

The menu currently takes a tile's shape from a per-system table (src/library/boxart.h), with a
`menu/boxart.ini` on the card to override it per region. The alternative is to read the shape off
the cover itself and snap it to a supported set, which needs no table and no setting and copes
with a card that mixes regions.

Whether that works is a property of real art packs, not of the code, and the honest answer is
that it depends on how cleanly source aspects cluster. Scraper output is not
dimension-normalised: some scans are tight, some carry margin, some are padded square, and some
"covers" are title screens or cartridge photos. The one measurement this project has -- a 40-card
stratified sample of n64-flashcart-menu-metadata -- found 11 of 40 portrait and 25 of 40 more
than 0.05 off the aspect the asset spec asks for.

So this tree carries a mixture whose composition is stated rather than discovered, and the
histogram it prints is the input to that decision:

  --mix realistic   the default. Mostly right, with the failures a real pack contains.
  --mix true        every cover exactly its system's box aspect. What the ideal looks like.
  --mix hostile     half the card wrong, for finding out what breaks rather than what it
                    usually looks like.

`--mix true` is the control. A layout that only looks compact on it is a layout that only works
on art nobody has.

Everything is derived from the title string, so two runs produce byte-identical output.

  tools/mksample.py -o build/sample
  make SAMPLE=1 sc64 -j8
"""

import argparse
import os
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from mkfixture import build_header, check_code_for, emit, harvest_games, make_ipl3  # noqa: E402
import mkplaystate                                                    # noqa: E402
import mkdemo                                                        # noqa: E402

# --------------------------------------------------------------------------- the library

# Titles are generated rather than hand-written, because a hundred and fifteen of them is past
# the point where a table is worth reading. Two word lists and a deterministic pairing: every
# name is obviously invented, none is a real game, and the pairing is a function of the index so
# a rebuild produces the same card.
FIRST = [
    "Amber", "Basalt", "Cinder", "Dune", "Ember", "Frost", "Glass", "Harbour",
    "Ion", "Jade", "Kelp", "Lantern", "Moth", "Night", "Obsidian", "Paper",
    "Quarry", "Rust", "Solar", "Thistle", "Umber", "Vellum", "Wax", "Zinc",
    "Copper", "Slate", "Pewter", "Cobalt", "Saffron", "Indigo", "Ochre", "Verdigris",
]
SECOND = [
    "Drift", "Rally", "Vale", "Skipper", "Circuit", "Line", "Meridian", "Watch",
    "Ballet", "Engine", "Blues", "Reef", "Marrow", "Bus", "League", "Sparrow",
    "Kings", "Pilgrim", "Tide", "Down", "Gate", "Sky", "Heist", "Garden",
    "Lantern", "Harvest", "Signal", "Parade", "Anthem", "Quarry", "Ferry", "Bloom",
]

SCENE_NAMES = list(mkdemo.SCENES.keys())

# Tab sizes, chosen so every tab is several full rows rather than one short one. A tab with four
# titles in it tells you nothing about how a grid looks.
#
# They were multiples of five, back when five columns was the only layout. A tab is four or five
# wide now depending on what shape its covers turn out to be -- and on a --mix that is not `true`
# a tab's shape is not knowable from here anyway -- so these are left as they were rather than
# retuned to a number that would only be right for one of the two. The measurements in AUDIT.md
# 1ak are against this exact 115-cover card and changing the counts would silently invalidate
# them. A partial last row is what most real tabs have.
SYSTEMS = [
    ("n64",  ".z64", 30),
    ("nes",  ".nes", 20),
    ("snes", ".sfc", 20),
    ("gb",   ".gb",  15),
    ("gbc",  ".gbc", 15),
    ("sms",  ".sms", 15),
]

# What shape the box actually is, matching boxart.c's built-in table.
TRUE_ASPECT = {
    "n64": 127 / 181.0, "nes": 127 / 180.0, "snes": 127 / 181.0,
    "sms": 128 / 179.0, "gb": 1.0, "gbc": 1.0,
}

# The mixtures. Each is a list of (weight, kind), and the kinds are the failure modes a real
# pack contains rather than a spread of arbitrary numbers:
#
#   true      the box aspect, within a few percent -- a tight scan
#   margin    the box aspect with 6-10 % of white space around it, which is most scanner output
#   square    padded to 1:1, which is what a pipeline that normalises to a square thumbnail does
#   card      1.4286 landscape, the old title-card spec and also what a screenshot looks like
#   tall      0.66, a PAL box or a scan that cropped the margins off the sides only
#   odd       something nobody planned: a spine included, or a cartridge photo
MIXES = {
    "true":      [(1, "true")],
    "realistic": [(62, "true"), (18, "margin"), (8, "square"), (7, "card"), (3, "tall"), (2, "odd")],
    "hostile":   [(30, "true"), (10, "margin"), (20, "square"), (25, "card"), (10, "tall"), (5, "odd")],
}


def seeded(title, salt):
    """A stable pseudo-random 32-bit value for a title, avalanched.

    The avalanche is not decoration. This was `crc32(salt + title)` and nothing else, and over the
    115 generated titles `crc32 % 100` never once landed in 80..99 -- so `square`, `card`, `tall`
    and `odd` had a combined weight of 20 % and were generated ZERO times. The whole corpus came
    out 76 true and 39 margin, and it would have looked entirely reasonable: mostly-correct art
    with some margin on it is exactly what the mix is meant to produce most of.

    CRC32 is linear, and its residues mod a small number stay correlated across inputs that
    differ in a few bytes -- which "Amber Drift", "Basalt Skipper", "Cinder Vale" all do. A test
    corpus that silently omits the cases it exists to contain is worse than no corpus, because
    every measurement taken against it agrees with itself.

    fmix32 out of MurmurHash3, which is the standard fix and costs four operations.
    """
    h = zlib.crc32((salt + "\x00" + title).encode()) & 0xFFFFFFFF
    h ^= h >> 16
    h = (h * 0x7FEB352D) & 0xFFFFFFFF
    h ^= h >> 15
    h = (h * 0x846CA68B) & 0xFFFFFFFF
    h ^= h >> 16
    return h


def assign_kinds(titles, mix):
    """Hand out cover kinds by exact quota, not by sampling.

    A corpus is not a population, and sampling it is the wrong instinct. Even after fixing the
    hash, drawing 115 independent samples against a 3 %% weight produced `tall` zero times -- so
    `--mix realistic` claimed to contain a PAL-shaped cover and contained none. Rounding the
    weights into counts and dealing them out means the tree contains exactly what the mix says,
    every time, at every --scale.

    The order is a hash permutation rather than the generated order, so the kinds interleave
    across systems instead of every odd cover landing in the SMS tab.
    """
    table = MIXES[mix]
    total = sum(w for w, _ in table)
    order = sorted(titles, key=lambda t: seeded(t, "deal"))

    counts, given = [], 0
    for i, (w, kind) in enumerate(table):
        n = len(order) - given if i == len(table) - 1 else (len(order) * w) // total
        counts.append((kind, n))
        given += n

    out, at = {}, 0
    for kind, n in counts:
        for t in order[at:at + n]:
            out[t] = kind
        at += n
    return out


def source_aspect(title, system, kind):
    """The width/height the cover is DRAWN at, which is not necessarily the box's."""
    true = TRUE_ASPECT[system]
    # Jitter in tenths of a percent, so a "true" cover is still not two covers with identical
    # dimensions -- real scans never are, and a snapping rule that only works on exact values
    # is not a rule.
    jitter = 1.0 + ((seeded(title, "jitter") % 61) - 30) / 1000.0

    if kind == "true":
        return true * jitter
    if kind == "margin":
        # Margin on all four sides pulls the aspect towards 1:1 by however much was added.
        pad = 0.06 + (seeded(title, "pad") % 5) / 100.0
        return ((1.0 + pad) / (1.0 + pad * true)) * true * jitter
    if kind == "square":
        return 1.0
    if kind == "card":
        return 1.4286
    if kind == "tall":
        return 0.66 * jitter
    # odd: two shapes nobody designed for, alternating so both appear
    return 1.82 if seeded(title, "odd") % 2 else 0.52


# Drawn at a fixed height and whatever width the aspect asks for, so a wider cover carries more
# picture rather than the same picture squashed. Height rather than width because the tallest
# case is the one that has to stay under Pillow's patience at 6x supersampling.
ART_H = 310


def art_pixels(aspect):
    w = int(round(ART_H * aspect))
    return max(32, w), ART_H


# --------------------------------------------------------------------------- generation

def titles_for(system, count, offset):
    """@p count names, unique across the whole card.

    The pairing must be a bijection onto FIRST x SECOND, and the obvious version is not. This was
    `FIRST[n % 32]` with `SECOND[(n * 7 + 3) % 32]`, on the reasoning that 7 is coprime with 32 so
    the second word strides. It does -- with period 32, the same as the first word, so the PAIR
    repeats every 32 and 115 titles came out as 32 distinct names each used three or four times.
    Duplicate filenames across systems, and a playstate keyed on a name that means several games.

    Striding the flattened index instead gives period 1024. 37 is coprime with 1024, so n -> pair
    is one-to-one over the whole card and consecutive titles still share no word.
    """
    out = []
    for i in range(count):
        n = ((offset + i) * 37) % (len(FIRST) * len(SECOND))
        out.append("%s %s" % (FIRST[n % len(FIRST)], SECOND[n // len(FIRST)]))
    return out


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="build/sample")
    ap.add_argument("--mix", choices=sorted(MIXES), default="realistic",
                    help="how often a cover is the wrong shape (default: realistic)")
    ap.add_argument("--scale", type=int, default=0,
                    help="fraction of the full library to build, as a percentage; 25 is a quick "
                         "look, 100 (the default) is a full card")
    args = ap.parse_args()

    root = args.output
    ipl3 = make_ipl3(None)
    taken = {code for code, _t in harvest_games(os.path.join(REPO, "src", "menu", "rom_info.c"))}

    scale = args.scale if args.scale else 100
    hist = {}
    n_art = 0
    n_jpeg = 0
    offset = 0
    by_system = {}          # system -> [playstate key, in generated order]

    # Names first, kinds second. The quota deal needs the whole title list before it can hand
    # anything out, so the library is enumerated once up front rather than as it is written.
    plan = []
    for system, ext, count in SYSTEMS:
        count = max(5, (count * scale) // 100)
        plan.append((system, ext, titles_for(system, count, offset)))
        offset += count
    every = [t for _s, _e, names in plan for t in names]
    kinds = assign_kinds(every, args.mix)
    # Dealt, not sampled, for the same reason the kinds are: `hash % 4` gave 42 of 115 rather than
    # the quarter it reads as, and a corpus should contain what it says it contains.
    jpegs = set(sorted(every, key=lambda t: seeded(t, "fmt"))[:len(every) // 4])

    for system, ext, names in plan:
        by_system[system] = []

        for title in names:
            if system == "n64":
                code = mkdemo.game_code_for(title, taken)
                taken.add(code)
                body = build_header(code, "", 0, ipl3)
                # playstate_key() takes the header check code for an N64 ROM and only falls back
                # to the filename hash when there is none. See write_playstate().
                by_system[system].append(check_code_for(code))
            else:
                body = bytes((j * 13 + 0x20) & 0xFF for j in range(2048))
                by_system[system].append(mkplaystate.fnv1a64(title + ext))
            emit(root, os.path.join("roms", system, title + ext), body)

            kind = kinds[title]
            aspect = source_aspect(title, system, kind)
            hist[kind] = hist.get(kind, 0) + 1
            w, h = art_pixels(aspect)

            # Art beside the ROM, which is thumbcache's rule 2 -- the only rule that reaches an
            # emulated-system title, and perfectly good for an N64 one.
            label = {"n64": "NINTENDO 64", "nes": "NES", "snes": "SUPER NES",
                     "gb": "GAME BOY", "gbc": "GAME BOY COLOR", "sms": "MASTER SYSTEM"}[system]
            card = mkdemo.draw_card(
                title,
                SCENE_NAMES[seeded(title, "scene") % len(SCENE_NAMES)],
                seeded(title, "pal") % len(mkdemo.PALETTES),
                mkdemo.STUDIOS[seeded(title, "studio") % len(mkdemo.STUDIOS)],
                label, (w, h))

            # A quarter of the card is JPEG -- dealt, see below -- because the shape probe reads a JPEG by walking its
            # marker chain and a PNG by reading a fixed offset -- two entirely different pieces of
            # code, only one of which is easy. If the JPEG walk is wrong the probe returns an
            # error, the record falls back to its system's box shape, and the grid looks fine; the
            # only way to notice is to have JPEGs on the card.
            #
            # Baseline, and quality 88 rather than the default, because the covers are flat
            # gradients and ringing on a flat gradient is the one artefact that would make somebody
            # think the DECODER was wrong.
            if title in jpegs:
                card.save(os.path.join(root, "roms", system, title + ".jpg"),
                          quality=88, progressive=False)
                n_jpeg += 1
            else:
                card.save(os.path.join(root, "roms", system, title + ".png"))
            n_art += 1

    emit(root, os.path.join("mainmenu", "config.ini"),
         b"[menu]\ndefault_directory=/roms\npal60_enabled=false\n")
    emit(root, os.path.join("saves", ".gitkeep"), b"")
    for core in ("neon64bu.rom", "lithium64.z64", "gb.v64", "gbc.v64", "smsPlus64.z64"):
        emit(root, os.path.join("mainmenu", "emulators", core),
             bytes((j * 31 + 0x10) & 0xFF for j in range(4096)))

    # A play history that makes Recent mixed on purpose. Two titles from each of the six
    # systems, so twelve played and Recent is two full rows and a bit -- and four of the twelve
    # are square, which is the case Recent exists to exercise. Recent and Favourites are the only
    # tabs where two tile shapes share a grid; a sample card that could not produce one would be
    # missing the layout's whole reason for being what it is.
    keys = []
    for take in (0, 1):
        for system, _ext, _n in SYSTEMS:
            ks = by_system[system]
            if take < len(ks):
                # Every third one a favourite, so Favourites is mixed too and is not simply a
                # copy of Recent.
                keys.append((ks[take], len(keys) % 3 == 0))
    write_playstate(os.path.join(root, "mainmenu", "cache", "playstate.dat"), keys)

    total = sum(os.path.getsize(os.path.join(d, f))
                for d, _, fs in os.walk(root) for f in fs)
    order = ["true", "margin", "square", "card", "tall", "odd"]
    spread = "  ".join("%s %d" % (k, hist[k]) for k in order if k in hist)
    print("sample: %d titles, %d covers (%d jpeg), mix=%s, %.1f MB -> %s"
          % (n_art, n_art, n_jpeg, args.mix, total / (1024.0 * 1024.0), root))
    print("cover shapes: " + spread)
    return 0


# 2026-08-04 14:30 UTC, the same wall clock a scripted run is pinned to in app.c. A play history
# stamped with the host's time would make "played 3 hours ago" a different string on every run.
BASE_EPOCH = 1785853800


def write_playstate(path, keys):
    """@p keys is a list of (key, favourite) in play order, oldest first.

    Keyed correctly per system, which is the trap mkplaystate.py's own comment records: an N64
    record keys on the header CHECK CODE and only falls back to the filename hash when there is
    none, so naming an N64 file here would write a record that can never match -- indistinguishable
    from the file being absent.
    """
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    entries = [[key, BASE_EPOCH + i * 3600, i + 1, 1 if fav else 0]
               for i, (key, fav) in enumerate(keys)]
    with open(path, "wb") as f:
        f.write(mkplaystate.build(entries))


if __name__ == "__main__":
    sys.exit(main())
