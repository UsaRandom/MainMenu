#!/usr/bin/env python3
"""Keep the vendored icon corpus honest about what it contains and who drew it.

`assets/icons/` is 3,894 SVGs of somebody else's artwork, CC BY 3.0, redistributed by this
repository. Two things about it have to stay true, and neither is visible from a build that
succeeds:

**Nothing on the blocklist is in the tree.** 286 of the corpus's 4,180 icons were left out because
their subject is recognisably someone else's property. That used to be enforced on every build:
`ICON_DIR` pointed at the whole corpus and `mkpack.py --exclude` dropped them as it packed, so the
exclusion was re-applied continuously and could not lapse. Vendoring applied it *once*, when the
files were copied. From then on a blocked icon can only come back by being added, and nothing
about the build would notice -- the pack would simply be one icon bigger. This is that lapse
turned back into a test.

**Every author in the tree is credited.** CC BY is an attribution licence and the artwork is here
under it. If an author directory exists that `assets/icons/README.md` does not count and
`docs/CREDITS.md` does not name, then the cartridge is redistributing that person's work without
saying so -- which is the one failure in this area that is not a bug but a breach.

## What this cannot check

That every line of `tools/ip-blocklist.txt` still names a real icon. `mkpack.py` warns when an
exclusion matches nothing, which caught typos and upstream renames while the whole corpus was on
disk. Against a tree the exclusions have already been applied to, a line that names nothing and a
line that did its job are the same line. Pass `--full-corpus DIR` pointing at an unfiltered
game-icons.net checkout to get that check back; without it, it is skipped and said to be skipped.

Run from tools/hosttest/run.sh. Needs no build and no emulator.
"""

import argparse
import os
import re
import sys

# The one author whose directory name is not their credited name. Kept as a table rather than
# matched by prefix: prefix matching also pairs `catsu` with `Cat` and would call a missing
# credit a match.
DIR_ALIASES = {"lucasms": "Lucas"}


def slug(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def read_tree(icons_dir):
    """{author: {name, ...}} for every .svg under a <author>/<name>.svg tree."""
    tree = {}
    for entry in sorted(os.listdir(icons_dir)):
        path = os.path.join(icons_dir, entry)
        if not os.path.isdir(path):
            continue
        names = {os.path.splitext(f)[0] for f in os.listdir(path) if f.endswith(".svg")}
        if names:
            tree[entry] = names
    return tree


def read_blocklist(path):
    out = set()
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(line)
    return out


def read_readme_counts(path):
    """{author: count} from the '| `author/` | Name | 1,234 |' rows of the attribution table."""
    text = open(path, encoding="utf-8").read()
    rows = re.findall(r"^\| `([a-z0-9-]+)/` \| .+? \| ([\d,]+) \|", text, re.M)
    return {author: int(count.replace(",", "")) for author, count in rows}


def read_credited_names(path):
    """The author names listed under the icon section of CREDITS.md."""
    text = open(path, encoding="utf-8").read()
    marker = "Icons made by"
    if marker not in text:
        return []
    tail = text.split(marker, 1)[1]
    names = []
    for line in tail.splitlines():
        m = re.match(r"^- (.+?)(?: \(CC0\))?(?: - \S+)?\s*$", line)
        if m:
            names.append(m.group(1).strip())
        elif names and not line.strip():
            break  # the list is one block; the first blank line after it ends it
    return names


# -- the checks, as functions over already-read data so the self-test can doctor the inputs --

def check_blocked_absent(tree, blocked):
    present = sorted(f"{a}/{n}" for a, names in tree.items() for n in names
                     if f"{a}/{n}" in blocked)
    return [f"{k} is on the blocklist and is in the tree" for k in present]


def check_counts(tree, counts):
    problems = []
    for author in sorted(set(tree) | set(counts)):
        have = len(tree.get(author, ()))
        said = counts.get(author)
        if said is None:
            problems.append(f"{author}/ has {have} icons and is not in the attribution table")
        elif author not in tree:
            problems.append(f"the attribution table counts {said} icons for {author}/, "
                            f"which does not exist")
        elif said != have:
            problems.append(f"the attribution table says {said} icons for {author}/, "
                            f"the tree has {have}")
    return problems


def check_credited(tree, names):
    known = {slug(n) for n in names}
    return [f"{a}/ is in the tree and nobody by that name is credited in CREDITS.md"
            for a in sorted(tree)
            if slug(DIR_ALIASES.get(a, a)) not in known]


def check_blocklist_rot(blocked, full_corpus):
    """Only possible against an unfiltered corpus; see the module docstring."""
    real = set()
    for dirpath, _dirnames, filenames in os.walk(full_corpus):
        author = os.path.basename(dirpath)
        for fn in filenames:
            if fn.endswith(".svg"):
                real.add(f"{author}/{os.path.splitext(fn)[0]}")
    return [f"{k} is on the blocklist and is not in the corpus -- renamed, or a typo"
            for k in sorted(blocked - real)]


def self_test():
    """Establish that each check can say no before a green run from it is believed."""
    tree = {"lorc": {"ace", "acorn"}, "delapouite": {"meeple"}}
    cases = [
        ("blocked icons in the tree",
         check_blocked_absent(tree, {"delapouite/meeple"})),
        ("an author missing from the attribution table",
         check_counts(tree, {"lorc": 2})),
        ("a wrong count in the attribution table",
         check_counts(tree, {"lorc": 99, "delapouite": 1})),
        ("an uncredited author",
         check_credited(tree, ["Lorc"])),
    ]
    for label, problems in cases:
        if not problems:
            print(f"iconcheck: SELF-TEST FAILED -- {label} was not detected, so a green result "
                  f"from this tool means nothing", file=sys.stderr)
            return False
    print(f"iconcheck: self-test ok ({len(cases)} planted faults correctly rejected)")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--icons", default=None, help="default <root>/assets/icons")
    ap.add_argument("--full-corpus", default=os.environ.get("ICON_FULL_DIR"),
                    help="an UNFILTERED game-icons.net tree, which enables the blocklist rot "
                         "check that the vendored tree cannot support")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test and not self_test():
        return 1

    root = args.root
    icons = args.icons or os.path.join(root, "assets", "icons")
    if not os.path.isdir(icons):
        print(f"iconcheck: {icons} is missing", file=sys.stderr)
        return 1

    tree = read_tree(icons)
    total = sum(len(v) for v in tree.values())
    blocked = read_blocklist(os.path.join(root, "tools", "ip-blocklist.txt"))

    problems = []
    problems += check_blocked_absent(tree, blocked)
    problems += check_counts(tree, read_readme_counts(os.path.join(icons, "README.md")))
    problems += check_credited(tree, read_credited_names(os.path.join(root, "docs", "CREDITS.md")))
    # The summary below states the conclusion, so it has to be printed from the result and not
    # from the intent. Printed unconditionally it read "all credited; none of the blocked icons
    # present" directly above the line saying delapouite/death-star was present.
    clean = not problems

    if args.full_corpus and os.path.isdir(args.full_corpus):
        problems += check_blocklist_rot(blocked, args.full_corpus)
        rot = f"{len(blocked)} blocklist entries checked against {args.full_corpus}"
    else:
        rot = (f"blocklist rot NOT checked -- pass --full-corpus with an unfiltered corpus "
               f"to check its {len(blocked)} entries still name real icons")

    if clean:
        print(f"iconcheck: {total} icons by {len(tree)} authors, all credited; "
              f"none of the {len(blocked)} blocked icons present")
    else:
        print(f"iconcheck: {total} icons by {len(tree)} authors, "
              f"{len(problems)} problems below")
    print(f"iconcheck: {rot}")

    for p in problems:
        print(f"iconcheck: {p}", file=sys.stderr)
    return 0 if clean else 1


if __name__ == "__main__":
    sys.exit(main())
