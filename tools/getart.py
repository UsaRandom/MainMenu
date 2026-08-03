#!/usr/bin/env python3
"""
Fetch real title-card art from n64-tools/n64-flashcart-menu-metadata for testing.

That repository is what people will actually put on their cards, so it is the corpus the menu
has to survive -- not the procedurally generated fixture art, which is uniform, correctly sized
and therefore useless for finding the cases that break.

It is Unlicense, so there is no restriction on using it. It is also **1.77 GB** across 1,672
PNGs at a mean of 1 MB each, so nothing here is committed and nothing is cloned wholesale. This
downloads a bounded subset into a gitignored cache and stops.

  tools/getart.py --list                 # what codes have art
  tools/getart.py --count 40             # fetch 40 into build/artcache
  tools/getart.py --codes NSME,NK4E,NZLE # fetch specific games

What the corpus actually contains, measured rather than assumed -- see docs/AUDIT.md:

  * mostly ~1000 px wide landscape scans, aspect around 1.43-1.47
  * some legacy 112x158 and 158x112 thumbnails from the stock menu's old asset size
  * a mean of 1 MB per file, which is 3.5x wider than the 280 x 196 asset spec

So real art is neither the size nor reliably the aspect the spec asks authors to deliver. The
menu has to scale and crop on the way into the cache; a decoder that only accepts 280 x 196
would reject essentially the whole corpus.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

REPO = "n64-tools/n64-flashcart-menu-metadata"
TREE = "https://api.github.com/repos/%s/git/trees/main?recursive=1" % REPO
RAW = "https://raw.githubusercontent.com/%s/main/%%s" % REPO

HERE = os.path.dirname(os.path.abspath(__file__))
REPOROOT = os.path.dirname(HERE)
DEFAULT_OUT = os.path.join(REPOROOT, "build", "artcache")


def fetch_index():
    """Every 4-char game code that has a boxart_front.png, with its byte size."""
    try:
        with urllib.request.urlopen(TREE, timeout=60) as r:
            tree = json.load(r)["tree"]
    except urllib.error.URLError as e:
        sys.exit("cannot reach GitHub: %s" % e)

    out = {}
    for e in tree:
        p = e["path"]
        if not p.endswith("/boxart_front.png") or not p.startswith("metadata/"):
            continue
        parts = p.split("/")
        if len(parts) == 6:                      # metadata/G/A/M/E/boxart_front.png
            out["".join(parts[1:5])] = (p, e.get("size", 0))
    return out


def png_size(path):
    """(width, height) from a PNG header, without decoding it."""
    import struct
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    return struct.unpack(">II", head[16:24])


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default=DEFAULT_OUT)
    ap.add_argument("--count", type=int, default=40,
                    help="how many to fetch, smallest first (default 40)")
    ap.add_argument("--codes", help="comma-separated game codes instead of --count")
    ap.add_argument("--list", action="store_true", help="print available codes and exit")
    ap.add_argument("--max-bytes", type=int, default=2_000_000,
                    help="skip files larger than this (default 2 MB)")
    args = ap.parse_args()

    index = fetch_index()
    print("%d games have boxart_front.png" % len(index), file=sys.stderr)

    if args.list:
        for code in sorted(index):
            print("%s %d" % (code, index[code][1]))
        return 0

    if args.codes:
        wanted = [c.strip().upper() for c in args.codes.split(",") if c.strip()]
        missing = [c for c in wanted if c not in index]
        if missing:
            print("no art for: %s" % ", ".join(missing), file=sys.stderr)
        wanted = [c for c in wanted if c in index]
    else:
        # Stratified across the size distribution, not smallest-first. Taking the cheapest
        # files returns nothing but the legacy 158x112 thumbnails, and a corpus of those would
        # have said the menu handles real art fine while never once exercising a 1000 px scan.
        pool = sorted((c for c in index if index[c][1] <= args.max_bytes),
                      key=lambda c: index[c][1])
        if args.count >= len(pool):
            wanted = pool
        else:
            step = len(pool) / float(args.count)
            wanted = [pool[int(i * step)] for i in range(args.count)]

    os.makedirs(args.output, exist_ok=True)
    got = bytes_got = 0
    sizes = {}

    for code in wanted:
        path, _ = index[code]
        dest = os.path.join(args.output, code[0], code[1], code[2], code[3],
                            "boxart_front.png")
        os.makedirs(os.path.dirname(dest), exist_ok=True)

        if not os.path.exists(dest):
            try:
                with urllib.request.urlopen(RAW % path, timeout=120) as r, \
                     open(dest, "wb") as f:
                    f.write(r.read())
            except urllib.error.URLError as e:
                print("  %s failed: %s" % (code, e), file=sys.stderr)
                continue

        got += 1
        bytes_got += os.path.getsize(dest)
        wh = png_size(dest)
        if wh:
            sizes[code] = wh

    print("%d cards, %.1f MB -> %s" % (got, bytes_got / 1e6, args.output))

    if sizes:
        import collections
        shape = collections.Counter("%dx%d" % wh for wh in sizes.values())
        print("dimensions: %s" % dict(shape.most_common(8)))
        asp = sorted(w / h for w, h in sizes.values())
        print("aspect: min %.3f  median %.3f  max %.3f  (spec is 1.4286)"
              % (asp[0], asp[len(asp) // 2], asp[-1]))
        odd = [c for c, (w, h) in sizes.items() if abs(w / h - 1.4286) > 0.05]
        print("%d of %d are more than 0.05 off the spec aspect" % (len(odd), len(sizes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
