#!/usr/bin/env python3
"""
Tile framebuffer dumps into one strip, for judging motion with your eyes.

    tools/contactsheet.py build/after/detail-sheet -o build/after/sheet.png
    tools/contactsheet.py build/after/*/ -o all.png --cols 4

Some defects are invisible in a single frame and obvious across a row of them: comb artefacts on
a scrolling grid of high-contrast box art, a tile popping in a frame late, an animation that eases
wrong, sub-pixel shimmer on a texture blit. None of those change a hash in a way that tells you
what happened -- the hash says "different", which is exactly as useful as "something moved".

So this is the other half of the harness. `regress.sh` answers "did anything change"; this answers
"is the change the one I wanted". Neither substitutes for the other, and the hash gate is the one
that runs unattended.

Frames are labelled with their index so a sheet can be talked about ("frame 3 is where the tile
pops") without counting along the row.
"""

import argparse
import glob
import os
import struct
import sys
import zlib


def read_png(path):
    """Minimal PNG reader for the 8-bit RGB files fbdump2png.py writes. (w, h, rows)."""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s is not a PNG" % path)

    pos = 8
    w = h = None
    idat = bytearray()
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            w, h, depth, color = struct.unpack(">IIBB", body[:10])
            if depth != 8 or color != 2:
                raise ValueError("%s: expected 8-bit RGB, got depth %d colour %d"
                                 % (path, depth, color))
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length

    raw = zlib.decompress(bytes(idat))
    stride = w * 3
    rows, prev = [], bytearray(stride)
    p = 0
    for _y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        # Undo the per-line filter. Only the five standard types exist.
        if f == 1:
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise ValueError("%s: unknown filter %d" % (path, f))
        rows.append(line)
        prev = line
    return w, h, rows


def write_png(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        raw += r
    out = [b"\x89PNG\r\n\x1a\n"]

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    out.append(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
    out.append(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
    out.append(chunk(b"IEND", b""))
    open(path, "wb").write(b"".join(out))


# 3x5 digits, so a frame index can be stamped without dragging in a font.
DIGITS = {
    "0": ["111", "101", "101", "101", "111"], "1": ["010", "110", "010", "010", "111"],
    "2": ["111", "001", "111", "100", "111"], "3": ["111", "001", "111", "001", "111"],
    "4": ["101", "101", "111", "001", "001"], "5": ["111", "100", "111", "001", "111"],
    "6": ["111", "100", "111", "101", "111"], "7": ["111", "001", "010", "010", "010"],
    "8": ["111", "101", "111", "101", "111"], "9": ["111", "101", "111", "001", "111"],
}


def stamp(rows, w, h, x, y, text, scale=2):
    for ch in text:
        pat = DIGITS.get(ch)
        if pat is None:
            x += 4 * scale
            continue
        for gy, line in enumerate(pat):
            for gx, bit in enumerate(line):
                if bit != "1":
                    continue
                for sy in range(scale):
                    for sx in range(scale):
                        px, py = x + gx * scale + sx, y + gy * scale + sy
                        if 0 <= px < w and 0 <= py < h:
                            i = px * 3
                            rows[py][i] = rows[py][i + 1] = rows[py][i + 2] = 255
        x += 4 * scale


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="PNG files, or directories of them")
    ap.add_argument("-o", "--output", default="contactsheet.png")
    ap.add_argument("--cols", type=int, default=0, help="0 = one row per input directory")
    ap.add_argument("--gap", type=int, default=4)
    args = ap.parse_args()

    files = []
    for spec in args.inputs:
        if os.path.isdir(spec):
            files += sorted(glob.glob(os.path.join(spec, "*.png")))
        else:
            files.append(spec)
    if not files:
        sys.exit("no PNGs found")

    frames = [read_png(f) for f in files]
    fw = max(f[0] for f in frames)
    fh = max(f[1] for f in frames)
    cols = args.cols if args.cols > 0 else len(frames)
    rows_n = (len(frames) + cols - 1) // cols

    W = cols * fw + (cols + 1) * args.gap
    H = rows_n * fh + (rows_n + 1) * args.gap
    sheet = [bytearray(W * 3) for _ in range(H)]

    for i, (w, h, rws) in enumerate(frames):
        cx = i % cols
        cy = i // cols
        ox = args.gap + cx * (fw + args.gap)
        oy = args.gap + cy * (fh + args.gap)
        for y in range(h):
            sheet[oy + y][(ox) * 3:(ox + w) * 3] = rws[y][:w * 3]
        stamp(sheet, W, H, ox + 4, oy + 4, str(i))

    write_png(args.output, W, H, sheet)
    print("%s  %dx%d  %d frames" % (args.output, W, H, len(frames)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
