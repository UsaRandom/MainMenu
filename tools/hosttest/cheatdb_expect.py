#!/usr/bin/env python3
"""
Read cheats.db independently of src/cheats/cheatdb.c and write down what it says.

    tools/hosttest/cheatdb_expect.py build/cheats.db build/hosttest/cheatdb-expect.txt

The point is that this parser and the C one are written from the spec rather than from each
other. A round-trip test where the writer and reader share code cannot catch an offset that is
wrong in both -- AUDIT.md already carries two harnesses that measured the wrong thing and looked
green doing it. Here mkcheatdb.py writes, this reads, and test_cheatdb.c reads again; the three
have to agree or one of them is wrong.

Output is one game per line, tab separated:

    game_code  version  group_count  code_count  first_group_name  addr0  val0  addrN  valN

with addrN/valN the LAST code line of the LAST group, so a blob that is right at the front and
truncated at the back cannot pass.
"""

import struct
import sys

INDEX_ROW = 24
GROUP_ROW = 8


def games(blob):
    magic, ver, _pad, count, index_off, blob_off, _s_off, _s_size, _crc = \
        struct.unpack(">IHHIIIIII", blob[:32])
    if magic != 0x4D363443:
        sys.exit("not a cheats.db: magic %08x" % magic)
    if ver != 2:
        sys.exit("expected format 2, found %d" % ver)

    for i in range(count):
        o = index_off + i * INDEX_ROW
        check, code, version, _flags, groups, off, size = \
            struct.unpack(">Q4sBBHII", blob[o:o + INDEX_ROW])
        yield check, code.decode("latin1"), version, groups, blob[blob_off + off:blob_off + off + size]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    blob = open(sys.argv[1], "rb").read()

    rows = []
    for _check, code, version, n, game in games(blob):
        hdrs = [struct.unpack(">IHH", game[i * GROUP_ROW:(i + 1) * GROUP_ROW]) for i in range(n)]
        total = max(f + c for _no, f, c in hdrs)
        codes_at = n * GROUP_ROW

        name_off, _f, _c = hdrs[0]
        end = game.index(b"\0", name_off)
        name = game[name_off:end].decode("utf-8", "replace")

        a0, v0 = struct.unpack(">II", game[codes_at:codes_at + 8])
        last = codes_at + (total - 1) * 8
        aN, vN = struct.unpack(">II", game[last:last + 8])

        rows.append("%s\t%d\t%d\t%d\t%s\t%08x\t%08x\t%08x\t%08x"
                    % (code, version, n, total, name, a0, v0, aN, vN))

    with open(sys.argv[2], "w", encoding="utf-8") as f:
        f.write("\n".join(rows))
        f.write("\n")
    print("  %d games described -> %s" % (len(rows), sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
