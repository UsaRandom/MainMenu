#!/usr/bin/env python3
"""Bake docs/CREDITS.md into the cartridge filesystem, and refuse to bake it wrong.

The credits screen is the one piece of UI whose text is *data* rather than string literals, which
means the charset check that protects every other screen cannot see it. A smart quote pasted into
the Markdown would reach the console as a hole in the licence text and nothing would have said so.

So this does two jobs, and the second is the one worth having:

  1. Flatten the Markdown into a line-tagged format the screen can render without a parser.
  2. Assert every character of the result is in the font's charset, and fail the build if not.

The tags are one byte at the start of each line:

    H  section heading      (from `# `)
    S  subsection heading   (from `## `)
    B  bullet               (from `- `)
    U  a bare URL on its own line
    T  body text
    -  a blank line, no content follows

The screen styles by tag and wraps with WRAP_WORD, so nothing here is wrapped or padded: line
lengths are the author's paragraphs, not a column count. That matters because the font is
proportional and any column count baked here would be wrong for some line.

Everything before the first horizontal rule is developer-facing front matter and is dropped, so
the file can carry instructions to whoever edits it without those reaching the console.
"""

import argparse
import re
import sys
from pathlib import Path

# rdpq_text treats these as markup and asserts on a malformed pair -- see ui_text()'s
# escape_markup(). ui_text escapes them at draw time, so they would survive; they are rejected
# here anyway because there is no legitimate reason for one to appear in a credits file and a
# doubled glyph on screen is a bug nobody would think to look for.
MARKUP = "$^"


def load_charset(path: Path) -> set:
    """Every character the font can draw. Whitespace in the charset file is separator, not data."""
    chars = set(path.read_text(encoding="utf-8"))
    chars.discard("\n")
    # Space is not listed in charset.txt but mkfont always bakes it, and every line here has one.
    chars.add(" ")
    return chars


def flatten(md: str) -> list:
    """Markdown to tagged lines. Deliberately not a Markdown parser -- see the format rules in
    docs/CREDITS.md, which exist so this can stay thirty lines long."""
    lines = md.splitlines()

    # Drop the front matter: everything up to and including the first horizontal rule.
    for i, line in enumerate(lines):
        if line.strip() == "---":
            lines = lines[i + 1:]
            break
    else:
        raise SystemExit("mkcredits: no `---` rule found; the front matter has no end marker")

    out = []

    def emit(tag_line):
        out.append(tag_line)

    def blank():
        # Collapse runs of blanks, and never open with one.
        if out and out[-1] != "-":
            out.append("-")

    # Consecutive body lines are one paragraph. The source is hard-wrapped at 100 columns for
    # people editing it; the console wraps with WRAP_WORD at whatever width it has. Emitting the
    # source's line breaks would hand the renderer 100-column fragments to re-wrap, and every
    # paragraph would come out ragged at a width nobody chose.
    para = []

    def flush():
        if para:
            emit("T" + " ".join(para))
            para.clear()

    for raw in lines:
        line = raw.rstrip()
        # Bold and code markers carry no meaning once the tag decides the style.
        line = re.sub(r"\*\*(.+?)\*\*", r"\1", line)
        line = re.sub(r"`(.+?)`", r"\1", line)
        stripped = line.strip()

        if stripped == "" or stripped == "---":
            flush()
            blank()
        elif line.startswith("## "):
            flush()
            emit("S" + stripped[3:])
        elif line.startswith("# "):
            flush()
            # A section always gets air above it, even if the source did not leave any.
            blank()
            emit("H" + stripped[2:])
        elif stripped.startswith("- "):
            flush()
            emit("B" + stripped[2:])
        elif "://" in stripped and " " not in stripped:
            flush()
            emit("U" + stripped)
        else:
            para.append(stripped)
    flush()

    while out and out[-1] == "-":
        out.pop()
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("source", type=Path, help="docs/CREDITS.md")
    ap.add_argument("-o", "--out", type=Path, required=True, help="output .txt for the DFS")
    ap.add_argument("--charset", type=Path, required=True, help="assets/fonts/charset.txt")
    args = ap.parse_args()

    lines = flatten(args.source.read_text(encoding="utf-8"))
    allowed = load_charset(args.charset)

    # The check, and the reason this script exists. Report every offender rather than the first,
    # because they arrive in batches when text is pasted from somewhere.
    bad = []
    for n, line in enumerate(lines, 1):
        for col, ch in enumerate(line[1:], 1):
            if ch in MARKUP:
                bad.append((n, col, ch, "rdpq markup character"))
            elif ch not in allowed:
                bad.append((n, col, ch, "not in the font charset"))
    if bad:
        for n, col, ch, why in bad[:40]:
            print(f"mkcredits: {args.source}: line {n} col {col}: "
                  f"U+{ord(ch):04X} {ch!r} -- {why}", file=sys.stderr)
        if len(bad) > 40:
            print(f"mkcredits: ... and {len(bad) - 40} more", file=sys.stderr)
        print(f"mkcredits: {len(bad)} character(s) the console cannot draw. "
              f"Nothing written.", file=sys.stderr)
        return 1

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"    [CREDITS] {args.out} ({len(lines)} lines, {args.out.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
