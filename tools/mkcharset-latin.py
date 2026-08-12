#!/usr/bin/env python3
"""Derive the Latin-only body charset from the full one.

The body font is the largest allocation this program makes: 2,697 characters baked at 20 px with
an outline comes to **1,284,208 bytes of RDRAM**, measured, which is more than the two framebuffers
it shares a 4 MB heap with. 2,187 of those characters are CJK ideographs and another 171 are kana
and CJK punctuation. A console with no Expansion Pak cannot have them and cannot have a menu; this
is what it gets instead.

Derived rather than committed, because a second charset file checked in beside the first is a file
that drifts. Everything here comes out of charset.txt, so a character added there is either carried
into this one or is deliberately in the dropped set -- and the count of each is printed so the
build says what it did.

    tools/mkcharset-latin.py -i assets/fonts/charset.txt -o build/charset-latin.txt

What is kept: everything below U+2E80, which is ASCII, Latin-1, Latin Extended-A and -B, the
punctuation and the handful of symbols. What is dropped: CJK radicals, ideographs, kana and the
CJK punctuation block. A dropped character draws as a hole -- see the note on charset-ui.txt in the
Makefile -- so a Japanese title on a 4 MB console shows its Latin part and gaps. That is a real
loss and it is the whole reason the full charset stays the default whenever there is RAM for it.
"""

import argparse
import sys

# U+2E80 is the start of CJK Radicals Supplement. Everything this menu draws in a game title that
# is not CJK sits below it, and everything above it is what does not fit.
CJK_START = 0x2E80


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-i", "--input", required=True, help="the full charset")
    ap.add_argument("-o", "--output", required=True, help="where to write the Latin subset")
    args = ap.parse_args()

    with open(args.input, encoding="utf-8") as f:
        text = f.read()

    kept, dropped = [], 0
    seen = set()
    for ch in text:
        if ch.isspace():
            continue
        if ord(ch) >= CJK_START:
            dropped += 1
            continue
        if ch in seen:
            continue
        seen.add(ch)
        kept.append(ch)

    if not kept:
        print("mkcharset-latin: nothing kept -- refusing to write an empty charset",
              file=sys.stderr)
        return 1

    # Sorted, so the file is stable whatever order the source happens to be in: an unstable
    # derived file rebakes the font on every build and changes the ROM for no reason.
    kept.sort()
    with open(args.output, "w", encoding="utf-8") as f:
        f.write("".join(kept))
        f.write("\n")

    print(f"mkcharset-latin: {len(kept)} characters kept, {dropped} CJK dropped -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
