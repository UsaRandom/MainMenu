#!/usr/bin/env python3
"""Refuse to build a string the font cannot draw.

Three of the five faces this program loads carry a restricted charset -- 84 glyphs of letters,
digits and marks rather than the body font's 7,931 -- because a full-charset bake at 40 px is
about 2.7 MB of glyphs against 681 KB at 20 px. See docs/GOTCHAS-PROFILES.md section 1.

The cost of that decision is silent. A character outside the charset does not fail to compile and
does not fail at run time: `mkfont` simply never baked it, and `rdpq_text` draws nothing where it
should be. What ships is a hole in the middle of a word, on a console, in whichever string nobody
re-read after changing it.

So: find every string literal handed to ui_text_font() with one of the restricted faces, and check
each character against that face's charset file. A miss is a build error.

## What this deliberately does not do

It is a regex over the source, not a parser, so it sees literals and not variables. A name typed
by a user, a category name out of icons.meta, a sprite name out of the pack -- none of those are
visible here, and none of them can be: they are data. Two of the three are covered elsewhere
(mkiconmeta.py caps display names at eight ASCII characters; the keyboard can only produce what it
can draw), and the third, the sprite name line, is the known gap. It is recorded here rather than
hidden because a checker that implied more coverage than it has would be worse than none.

Run from tools/hosttest/run.sh, which is cheap enough to run any time.
"""

import argparse
import re
import sys
from pathlib import Path

# Which face takes which charset. FNT_DEFAULT and FNT_BOOT are here too: the first is the full
# charset and effectively always passes, the second is the 41-glyph boot face and is the one this
# check was originally written for.
FACES = {
    "FNT_DEFAULT": "assets/fonts/charset.txt",
    "FNT_BOOT":    "assets/fonts/charset-boot.txt",
    "FNT_SMALL":   "assets/fonts/charset-ui.txt",
    "FNT_KEY":     "assets/fonts/charset-ui.txt",
    "FNT_FIELD":   "assets/fonts/charset-ui.txt",
}

# ui_text_font(FACE, ..., "a literal") and ui_text_wrap(FACE, ..., "a literal"). The string is the
# last argument, so anchor on the closing paren rather than counting commas -- the middle arguments
# contain arithmetic with commas in it.
CALL = re.compile(
    r'\bui_text_(?:font|wrap)\s*\(\s*(FNT_[A-Z]+)\b[^;]*?"((?:[^"\\]|\\.)*)"\s*\)', re.S)


def load_charset(path: Path) -> set:
    chars = set(path.read_text(encoding="utf-8"))
    chars.discard("\n")
    chars.add(" ")   # mkfont always bakes it; it is not listed
    return chars


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("root", nargs="?", default=".", type=Path)
    ap.add_argument("--self-test", action="store_true",
                    help="check a string that must fail, to prove the check works")
    args = ap.parse_args()

    charsets = {}
    for face, rel in FACES.items():
        p = args.root / rel
        if not p.exists():
            print(f"charsetcheck: {p} is missing", file=sys.stderr)
            return 1
        charsets[face] = load_charset(p)

    if args.self_test:
        bad = [c for c in "Who's playing?" if c not in charsets["FNT_BOOT"]]
        if not bad:
            print("charsetcheck: SELF-TEST FAILED -- the boot charset accepted lowercase, so a "
                  "green result from this tool means nothing", file=sys.stderr)
            return 1
        print(f"charsetcheck: self-test ok ({len(bad)} characters correctly rejected "
              f"by the boot charset)")

    problems = 0
    checked = 0

    # The keyboard's own tables, which the regex above cannot see: its glyphs reach ui_text_font()
    # through draw_key() as a variable, and its DELETE / SPACE / DONE labels do the same. They are
    # also the highest-risk strings in the program -- a glyph the keyboard offers but the font
    # cannot draw is a blank key that types an invisible character into somebody's name.
    kb = args.root / "src/screens/screen_keyboard.c"
    if kb.exists():
        src_text = kb.read_text(encoding="utf-8")
        # One trailing number, not two. The rows carried an absolute Y until the digit row was
        # added at the Y the handoff gave the first row and ended up four pixels under the text
        # field; the Y is computed now and the table holds only an X. This checker went red on the
        # shape change rather than silently checking nothing, which is the only reason the count
        # below exists -- keep the `if not rows` guard whatever the table becomes next.
        rows = re.findall(r'\{\s*"([A-Z0-9\-_.,:/()+&]+)"\s*,\s*\d+\s*\}', src_text)
        labels = re.findall(r'draw_key\([^;]*?"([A-Za-z]+)"', src_text)
        # The rows are written in capitals and the case key draws and types the fold of them, so
        # a-z is as much "what the keyboard offers" as A-Z is. Checking only the literal would
        # pass a font that can draw every key you can see and none of the ones you get.
        for s in rows + [s.lower() for s in rows] + labels:
            checked += 1
            for ch in s:
                if ch not in charsets["FNT_KEY"]:
                    print(f"charsetcheck: {kb}: the keyboard offers U+{ord(ch):04X} {ch!r} "
                          f"but FNT_KEY cannot draw it", file=sys.stderr)
                    problems += 1
        if not rows:
            # A silent zero here would mean the tables were renamed and this stopped checking
            # anything, which is the failure mode a checker must not have.
            print("charsetcheck: found no keyboard rows to check -- has the table changed shape?",
                  file=sys.stderr)
            problems += 1
    for src in sorted((args.root / "src").rglob("*.c")):
        text = src.read_text(encoding="utf-8")
        for m in CALL.finditer(text):
            face, literal = m.group(1), m.group(2)
            if face not in charsets:
                print(f"charsetcheck: {src}: unknown face {face}", file=sys.stderr)
                problems += 1
                continue
            # Undo the C escapes that matter for this check.
            s = literal.replace('\\n', '\n').replace('\\t', '\t').replace('\\"', '"')
            checked += 1
            line = text[:m.start()].count("\n") + 1
            for ch in s:
                if ch in "\n\t":
                    continue
                if ch not in charsets[face]:
                    print(f"charsetcheck: {src}:{line}: {face} cannot draw "
                          f"U+{ord(ch):04X} {ch!r} in {literal!r}", file=sys.stderr)
                    problems += 1

    if problems:
        print(f"charsetcheck: {problems} character(s) no baked font can draw", file=sys.stderr)
        return 1
    print(f"charsetcheck: {checked} literal(s) across the restricted faces, all drawable")
    return 0


if __name__ == "__main__":
    sys.exit(main())
