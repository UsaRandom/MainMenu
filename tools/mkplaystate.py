#!/usr/bin/env python3
"""Write a menu/cache/playstate.dat by hand, so the read path can be tested under ares.

The menu cannot create this file on the machine it is developed on: ares exposes the ROM's own
DFS as storage and the DFS is read-only, so `cache_writable()` is false and every writer
short-circuits. **Reads are a different matter.** A file baked into the fixture tree is a file the
menu will happily open, so the entire load half -- header, CRC, record matching, key derivation --
can be exercised for real by generating one here.

That is worth more than it sounds. Without it, "the opening tab is the first non-empty one" can
only ever be observed falling back to N64, because Recent and Favourites are unreachable when
nothing can be persisted. With it, the interesting branch is testable.

Keys: playstate keys on the ROM's 64-bit check code, and falls back to FNV-1a of the bare filename
for records with no usable header -- which is every emulated-system title, since only N64 headers
are parsed. This tool uses the filename path, because it is the one a host script can compute
without parsing ROM headers, and it is exercised by the SNES titles in the fixture.

Byte order is BIG-ENDIAN throughout: the file is written and read by an N64 and never travels.

    tools/mkplaystate.py -o build/fixture/menu/cache/playstate.dat \\
        --played "Super Mario World.smc" --favorite "Donkey Kong Country.smc"
"""

import argparse
import os
import struct
import zlib

MAGIC = 0x4D363450          # 'M64P'
FORMAT_VER = 1              # must equal MENU_CACHE_FORMAT_VER in src/library/cache.h
LIBF_FAVORITE = 1 << 0


def fnv1a64(s: str) -> int:
    """Must match cache_hash64() in src/library/cache.c exactly."""
    h = 1469598103934665603
    for b in s.encode("utf-8"):
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def record(key: int, last_played: int, play_count: int, flags: int) -> bytes:
    """ps_record_t: 24 bytes, packed, big-endian."""
    return struct.pack(">QIIHHI", key, last_played, play_count, flags, 0, 0)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--played", action="append", default=[],
                    help="bare ROM filename to mark as recently played; repeatable, "
                         "earliest first so the last one given is the most recent")
    ap.add_argument("--favorite", action="append", default=[],
                    help="bare ROM filename to mark as a favourite; repeatable")
    ap.add_argument("--base-time", type=int, default=1_600_000_000,
                    help="unix time of the earliest play")
    args = ap.parse_args()

    entries: dict[int, list] = {}

    def entry(name: str):
        k = fnv1a64(name)
        return entries.setdefault(k, [k, 0, 0, 0])

    for i, name in enumerate(args.played):
        e = entry(name)
        e[1] = args.base_time + i * 3600      # later in the list == more recent
        e[2] += 1

    for name in args.favorite:
        entry(name)[3] |= LIBF_FAVORITE

    if not entries:
        print("nothing to write; pass --played or --favorite")
        return 1

    payload = b"".join(record(*e) for e in entries.values())
    header = struct.pack(">IHHII", MAGIC, FORMAT_VER, 0, len(payload),
                         zlib.crc32(payload) & 0xFFFFFFFF)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(header + payload)

    print(f"{args.output}: {len(entries)} records, {len(payload) + len(header)} bytes")
    for name in args.played:
        print(f"  played    {name}  key={fnv1a64(name):#018x}")
    for name in args.favorite:
        print(f"  favourite {name}  key={fnv1a64(name):#018x}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
