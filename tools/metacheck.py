#!/usr/bin/env python3
"""Validate tools/icon-meta.jsonl against the corpus, or run a query against it.

    python3 tools/metacheck.py                 validate + summarise
    python3 tools/metacheck.py undead ghost    show what a search would return

Validation matters because the metadata is a separate artefact from the icons:
add or exclude an icon and the two drift apart silently, and a gap in a search
index is indistinguishable from an icon that does not exist. This checks that
every packed icon is tagged, that no tagged name has vanished from the corpus,
and that every category is one of the closed set documented in
docs/ICON-METADATA.md.

SPDX-License-Identifier: MIT
"""

import collections
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
META = os.path.join(HERE, "icon-meta.jsonl")
DOC = os.path.join(ROOT, "docs", "ICON-METADATA.md")


def load_corpus(icon_dir):
    """The icons the ROM actually packs, via mkpack's own logic rather than a
    reimplementation of it -- so this cannot disagree with the build."""
    spec = importlib.util.spec_from_file_location(
        "mkpack", os.path.join(HERE, "mkpack.py"))
    mk = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mk)
    entries = mk.collect([icon_dir], None)
    excluded = mk.load_exclusions(os.path.join(HERE, "ip-blocklist.txt"))
    return [n for n, _ in entries if n not in excluded]


def load_meta():
    recs, bad = [], []
    with open(META) as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
                assert isinstance(r["name"], str)
                assert isinstance(r["cat"], str)
                assert isinstance(r["tags"], list)
                recs.append(r)
            except Exception:
                bad.append(lineno)
    return recs, bad


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    recs, bad = load_meta()

    if args:                                  # query mode
        index = collections.defaultdict(set)
        for r in recs:
            for t in r["tags"]:
                index[t].add(r["name"])
            for w in re.split(r"[-/]", r["name"]):
                index[w].add(r["name"])
        for q in args:
            q = q.lower()
            exact = index.get(q, set())
            partial = {n for t, ns in index.items() if q in t for n in ns} - exact
            print(f"\n{q}  --  {len(exact)} exact, {len(partial)} partial")
            for n in sorted(exact)[:40]:
                print(f"   {n}")
            for n in sorted(partial)[:10]:
                print(f"   ~ {n}")
        return 0

    # Only the table under "## Categories". The doc has other tables whose
    # first column is also a backticked lowercase word, and scanning the whole
    # file silently admits those as valid category ids.
    doc = open(DOC).read()
    section = doc.split("## Categories", 1)[-1].split("\n## ", 1)[0]
    cats = set(re.findall(r"^\| `([a-z-]+)` \|", section, re.M))
    icon_dir = os.environ.get("ICON_DIR", os.path.join(ROOT, "..", "svgicons"))

    print(f"{len(recs)} records, {len(cats)} categories in the taxonomy")
    if bad:
        print(f"  malformed lines: {bad[:10]}")

    names = [r["name"] for r in recs]
    dupes = [n for n, c in collections.Counter(names).items() if c > 1]
    off = sorted({r["cat"] for r in recs} - cats)
    thin = [r["name"] for r in recs if len(r["tags"]) < 4]
    for label, items in (("duplicate names", dupes),
                         ("categories not in the taxonomy", off),
                         ("fewer than 4 tags", thin)):
        if items:
            print(f"  {label}: {len(items)}  {items[:8]}")

    if os.path.isdir(icon_dir):
        corpus = load_corpus(icon_dir)
        have = set(names)
        missing = [n for n in corpus if n not in have]
        stale = sorted(have - set(corpus))
        print(f"corpus {len(corpus)} icons: "
              f"{len(corpus) - len(missing)} tagged, {len(missing)} untagged")
        if missing:
            print(f"  untagged: {missing[:8]}")
        if stale:
            print(f"  tagged but not in the corpus: {len(stale)}  {stale[:8]}")
    else:
        print(f"corpus check skipped: ICON_DIR={icon_dir} not found")

    tagc = collections.Counter(t for r in recs for t in r["tags"])
    print(f"{len(tagc)} distinct tags, "
          f"{sum(tagc.values()) / max(1, len(recs)):.1f} per icon")

    novel = sum(1 for r in recs for t in r["tags"]
                if t not in r["name"].split("/")[1].replace("-", " "))
    print(f"{novel / max(1, sum(tagc.values())):.0%} of tags are not derivable "
          f"from the filename")

    ok = not (bad or dupes or off)
    print("OK" if ok else "PROBLEMS FOUND")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
