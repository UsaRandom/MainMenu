#!/usr/bin/env python3
"""Pack a directory tree of .svg files into a single random-access blob.

The demo ROM needs to reach any one of several thousand icons by index, and
doing that as several thousand individual DFS entries would mean a filesystem
lookup per icon plus the per-file overhead of the DFS directory itself. Instead
everything goes into one file with a flat index at the front: the ROM reads the
index once at boot into RAM, then any icon is a seek and a read.

Layout, all integers big-endian to match the console:

    magic    'SVGP'
    version  u32   = 1
    count    u32
    reserved u32
    entry[count]:  u32 data_off, u32 data_len, u32 name_off, u32 name_len
    names blob     (UTF-8, not terminated; lengths are in the index)
    data blob      (the .svg files verbatim)

Offsets are absolute from the start of the file so the ROM does no arithmetic
on them beyond the seek.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import struct
import sys


def collect(roots, limit=None):
    """Find .svg files, named '<parent-dir>/<stem>' and sorted for stable IDs."""
    found = []
    for root in roots:
        if os.path.isfile(root) and root.endswith(".svg"):
            found.append((os.path.basename(os.path.dirname(root)), root))
            continue
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if fn.endswith(".svg"):
                    found.append((os.path.basename(dirpath), os.path.join(dirpath, fn)))

    entries = []
    for author, path in found:
        stem = os.path.splitext(os.path.basename(path))[0]
        entries.append((f"{author}/{stem}", path))

    entries.sort(key=lambda e: e[0])
    if limit:
        entries = entries[:limit]
    return entries


def load_exclusions(path):
    """Read a blocklist of '<author>/<name>' entries, ignoring comments."""
    if not path:
        return set()
    out = set()
    with open(path) as fh:
        for line in fh:
            line = line.split("#", 1)[0].strip()
            if line:
                out.add(line)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("roots", nargs="+", help="directories or .svg files to pack")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--limit", type=int, default=0,
                    help="pack at most N icons (for quick development builds)")
    ap.add_argument("--exclude", metavar="FILE",
                    help="omit the '<author>/<name>' entries listed in FILE; "
                         "see tools/ip-blocklist.txt")
    args = ap.parse_args()

    entries = collect(args.roots, None)

    # Exclusions are applied before --limit, so capping the count for a quick
    # build cannot accidentally reintroduce something that was meant to be out.
    excluded = load_exclusions(args.exclude)
    if excluded:
        before = len(entries)
        entries = [e for e in entries if e[0] not in excluded]
        dropped = before - len(entries)
        unmatched = excluded - {e[0] for e in collect(args.roots, None)}
        print(f"  [PACK] excluded {dropped} icons via {args.exclude}")
        if unmatched:
            print(f"  [PACK] warning: {len(unmatched)} blocklist entries matched "
                  f"nothing, e.g. {sorted(unmatched)[:3]}")

    if args.limit:
        entries = entries[:args.limit]

    if not entries:
        sys.exit("mkpack: no .svg files found")

    count = len(entries)
    header_len = 16
    index_len = count * 16

    names = bytearray()
    name_spans = []
    for name, _path in entries:
        raw = name.encode("utf-8")
        name_spans.append((len(names), len(raw)))
        names += raw

    names_off = header_len + index_len
    data_off = names_off + len(names)

    data = bytearray()
    data_spans = []
    for _name, path in entries:
        with open(path, "rb") as fh:
            raw = fh.read()
        data_spans.append((data_off + len(data), len(raw)))
        data += raw

    out = bytearray()
    out += b"SVGP"
    out += struct.pack(">III", 1, count, 0)
    for (d_off, d_len), (n_off, n_len) in zip(data_spans, name_spans):
        out += struct.pack(">IIII", d_off, d_len, names_off + n_off, n_len)
    out += names
    out += data

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as fh:
        fh.write(out)

    biggest = max(d_len for _, d_len in data_spans)
    print(f"  [PACK] {count} icons, {len(out)/1024/1024:.2f} MB "
          f"(largest icon {biggest} bytes) -> {args.output}")


if __name__ == "__main__":
    main()
