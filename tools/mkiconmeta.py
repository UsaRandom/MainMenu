#!/usr/bin/env python3
"""Build the category index the icon picker browses, from the pack that actually shipped.

The picker is a list of categories beside a grid of icons, so it needs two things the pack does
not carry: which category each icon belongs to, and the icons of one category in a contiguous run
so a page is a slice rather than a search.

Both come from tools/icon-meta.jsonl, which tags every icon by hand -- 87% of those tags are not
derivable from the filename, which is the difference between a category list that means something
and one that groups `abstract-01` with `abstract-02`.

## Why this reads the pack rather than the metadata alone

The obvious version reads icon-meta.jsonl, sorts it, and writes the result. It is wrong whenever
the build is not the default one. `make ICON_LIMIT=200` packs 200 icons; `make ICON_EXCLUDE=`
packs 4180 including the 286 the IP review takes out. Counts baked from the full metadata against
either of those are a lie the position bar draws and the page counter prints.

So the pack is the authority: this reads the names out of it, looks each one up in the metadata,
and indexes what is really there. An icon in the pack with no metadata entry lands in `misc` and
is reported, rather than silently vanishing from every category and becoming unreachable.

## Output

    magic      'ICNM'
    version    u32 = 2
    cat_count  u32
    icon_count u32          -- total across all categories; equals the pack count
    starter[10]:     u16     -- a default face per profile slot; see STARTERS
    cat[cat_count]:  u16 first, u16 count, u16 name_off, u16 pad
    order[icon_count]: u16   -- pack index, grouped by category, sorted by name within
    strtab                   -- NUL-terminated display names

All big-endian. At 30 categories and 3894 icons this is about 8 KB, so it is read whole at boot
and never seeked again.

SPDX-License-Identifier: MIT
"""

import argparse
import json
import struct
import sys
from pathlib import Path

# Display names. The slugs are for the metadata file and are the wrong thing to put on screen
# twice over: `abstract-geometric` is 18 characters against a 132 px column at 24 px type, and
# nobody browsing for a picture of a dog is looking for `creature-animal`.
#
# Ordered deliberately rather than alphabetically. The first six are what somebody picking an
# identity actually reaches for, and burying `Animals` under `Abstract` because of a letter would
# be sorting for the machine's convenience.

# A default face per profile slot, so a new player is never a blank plate and ten of them are
# never the same one. Chosen from the handoff's own sample list where the corpus has the sprite
# and by hand where it does not -- there is no `star` or `heart` in game-icons under those names.
#
# A capped build (ICON_LIMIT) will not contain most of these, so an absent name falls back to
# something spread across whatever did get packed rather than to icon 0 ten times over.
STARTERS = [
    "lorc/crown",
    "caro-asercion/fox",
    "lorc/rocket",
    "sbed/shield",
    "lorc/moon",
    "carl-olsen/flame",
    "lorc/paw",
    "lorc/key",
    "caro-asercion/barn-owl",
    "delapouite/musical-notes",
]

CATEGORIES = [
    ("creature-animal",     "Animals"),
    ("creature-fantasy",    "Beasts"),
    ("weapon-melee",        "Blades"),
    ("weapon-ranged",       "Ranged"),
    ("armor-shield",        "Armour"),
    ("magic-alchemy",       "Magic"),
    ("symbol-ui",           "Symbols"),
    ("abstract-geometric",  "Shapes"),
    ("person-action",       "People"),
    ("person-role",         "Roles"),
    ("body-anatomy",        "Body"),
    ("creature-plant",      "Plants"),
    ("nature-weather",      "Weather"),
    ("nature-landscape",    "Scenery"),
    ("building-structure",  "Places"),
    ("vehicle-transport",   "Travel"),
    ("tech-scifi",          "Tech"),
    ("tool-craft",          "Tools"),
    ("weapon-explosive",    "Bombs"),
    ("food-drink",          "Food"),
    ("clothing-accessory",  "Clothes"),
    ("treasure-currency",   "Treasure"),
    ("container-storage",   "Boxes"),
    ("music-sound",         "Music"),
    ("knowledge-writing",   "Writing"),
    ("religion-occult",     "Occult"),
    ("game-sport",          "Games"),
    ("medical-health",      "Health"),
    ("time-measure",        "Time"),
    ("misc",                "Other"),
]


def read_pack_names(path: Path) -> list:
    """The names in the pack, in pack-index order. Mirrors the reader in src/ui/icon.c."""
    blob = path.read_bytes()
    if len(blob) < 16 or blob[:4] != b"SVGP":
        raise SystemExit(f"mkiconmeta: {path}: not an icons.pack")
    version, count = struct.unpack_from(">II", blob, 4)
    if version != 1:
        raise SystemExit(f"mkiconmeta: {path}: pack version {version}, expected 1")

    names = []
    for i in range(count):
        _off, _len, name_off, name_len = struct.unpack_from(">IIII", blob, 16 + i * 16)
        names.append(blob[name_off:name_off + name_len].decode("utf-8"))
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pack", type=Path, help="the icons.pack this build shipped")
    ap.add_argument("-m", "--meta", type=Path, required=True, help="tools/icon-meta.jsonl")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output icons.meta")
    args = ap.parse_args()

    names = read_pack_names(args.pack)

    cat_of = {}
    with args.meta.open(encoding="utf-8") as fh:
        for line in fh:
            d = json.loads(line)
            cat_of[d["name"]] = d["cat"]

    slug_index = {slug: i for i, (slug, _disp) in enumerate(CATEGORIES)}
    misc = slug_index["misc"]

    buckets = [[] for _ in CATEGORIES]
    untagged = 0
    unknown_slugs = set()
    for idx, name in enumerate(names):
        slug = cat_of.get(name)
        if slug is None:
            untagged += 1
            buckets[misc].append((name, idx))
        elif slug not in slug_index:
            unknown_slugs.add(slug)
            buckets[misc].append((name, idx))
        else:
            buckets[slug_index[slug]].append((name, idx))

    if untagged:
        # Not fatal. A build with ICON_EXCLUDE= packs 286 icons the metadata does not cover, and
        # refusing to build for that would make the escape hatch unusable. But it is said out
        # loud, because the same message with a default build means the metadata has rotted.
        print(f"mkiconmeta: {untagged} icon(s) have no metadata entry; filed under Other",
              file=sys.stderr)
    if unknown_slugs:
        raise SystemExit(f"mkiconmeta: metadata uses categories this tool does not know: "
                         f"{sorted(unknown_slugs)}. Add them to CATEGORIES.")

    # Sort within a category by name, so paging through one is stable and reproducible and two
    # builds of the same corpus produce the same file.
    order = []
    cats = []
    strtab = bytearray()
    for i, (_slug, disp) in enumerate(CATEGORIES):
        bucket = sorted(buckets[i])
        first = len(order)
        order.extend(idx for _name, idx in bucket)
        name_off = len(strtab)
        strtab += disp.encode("ascii") + b"\0"
        cats.append((first, len(bucket), name_off))

    if len(order) > 0xFFFF:
        raise SystemExit(f"mkiconmeta: {len(order)} icons will not fit a u16 index")

    # Starters, resolved against the pack that shipped rather than against the corpus.
    by_name = {name: i for i, name in enumerate(names)}
    starters = []
    missing = []
    for slot, want in enumerate(STARTERS):
        if want in by_name:
            starters.append(by_name[want])
        else:
            missing.append(want)
            # Spread across what is here, so a capped build still gives ten different faces.
            starters.append((slot * max(1, len(names) // len(STARTERS))) % max(1, len(names)))
    if missing:
        print(f"mkiconmeta: {len(missing)} starter icon(s) are not in this pack "
              f"({missing[0]}...); substituting from what is", file=sys.stderr)

    out = bytearray()
    out += b"ICNM" + struct.pack(">III", 2, len(CATEGORIES), len(order))
    for s in starters:
        out += struct.pack(">H", s)
    for first, count, name_off in cats:
        out += struct.pack(">HHHH", first, count, name_off, 0)
    for idx in order:
        out += struct.pack(">H", idx)
    out += strtab

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)

    empty = sum(1 for _f, c, _n in cats if c == 0)
    print(f"    [ICONMETA] {args.out} ({len(order)} icons, "
          f"{len(CATEGORIES) - empty} non-empty categories, {len(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
