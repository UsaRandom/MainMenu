#!/usr/bin/env python3
"""
Turn the framebuffer hex dumps in an ares log back into PNGs.

Adapted from lithium64's tool of the same name. The one substantive change: dimensions come
from the FBDUMP marker the ROM emits rather than from constants here. lithium64 could hardcode
280x239 because its resolution never moves; this project is still deciding between resolutions
and between interlaced and progressive, and a host script with a baked-in width silently
misreads every frame after such a change. The resulting images look like a rendering fault,
which is a genuinely expensive way to discover a parsing bug.

Frames come out of the emulator rather than off a screenshot, so what lands here is what the
RDP produced and not what a window manager or a scaler made of it.

  tools/fbdump2png.py build/run/ares.log -o build/run/frames
  tools/fbdump2png.py build/run/ares.log --hashes        # one sha256 prefix per frame
  tools/fbdump2png.py build/run/ares.log --vi -o out     # only the rows the VI scans out

--vi is the answer to a class of bug the dump cannot otherwise show. The framebuffer is 480 rows
and the VI output area is 240 lines, so VI_Y_SCALE is 2048/1024 -- two framebuffer rows per
scanline -- and with interlacing off the offset never alternates, so the odd rows are never
displayed. A dump reads RDRAM, so it shows every row and hashes a line nobody can see as though
it were on screen. --vi keeps the even rows only, which is what the console shows. It is opt-in
precisely so it does not silently rewrite every regression hash in the suite.
"""

import argparse
import hashlib
import os
import re
import struct
import sys
import zlib

# ares hex dump lines look like:
#   ffffffffa00f2300 0000: 00 11 22 ...  |........|
LINE = re.compile(r"^[0-9a-f]{8,16}\s+[0-9a-f]{4}:\s+((?:[0-9a-f]{2}[ ]+){1,16})")

# FBDUMP w=160 h=120 scale=4 fmt=rgba5551
MARKER = re.compile(r"FBDUMP\s+w=(\d+)\s+h=(\d+)\s+scale=(\d+)\s+fmt=(\w+)")


class Frame:
    def __init__(self, width, height, scale, fmt):
        self.width, self.height, self.scale, self.fmt = width, height, scale, fmt
        self.data = bytearray()

    @property
    def expected(self):
        return self.width * self.height * 2

    def complete(self):
        return len(self.data) >= self.expected


def extract(path):
    """Every frame between an FBDUMP marker and its FBEND."""
    frames, current = [], None
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")

            m = MARKER.search(line)
            if m:
                current = Frame(int(m.group(1)), int(m.group(2)),
                                int(m.group(3)), m.group(4))
                continue

            if "FBDUMP" in line and current is None:
                # A dump that announced itself without a parseable marker means the ROM and
                # this script disagree about the format. Say so rather than emitting nothing.
                print("warning: unparseable FBDUMP marker: %s" % line.strip(),
                      file=sys.stderr)
                continue

            if "FBEND" in line:
                if current is not None:
                    if not current.complete():
                        print("warning: frame %d truncated, %d of %d bytes"
                              % (len(frames), len(current.data), current.expected),
                              file=sys.stderr)
                    frames.append(current)
                current = None
                continue

            if current is None:
                continue

            m = LINE.match(line)
            if m:
                current.data += bytes(int(b, 16) for b in m.group(1).split())
    return frames


def to_rgb_rows(frame):
    """RGBA5551 big-endian to PNG-ready 8-bit RGB rows, one filter byte each."""
    if frame.fmt != "rgba5551":
        sys.exit("unsupported pixel format %r -- teach this script about it" % frame.fmt)

    data, w, h = frame.data, frame.width, frame.height
    rows = []
    for y in range(h):
        row = bytearray(b"\x00")             # filter type 0 (None)
        base = y * w * 2
        for x in range(w):
            i = base + x * 2
            if i + 1 >= len(data):
                row += b"\x00\x00\x00"
                continue
            px = (data[i] << 8) | data[i + 1]
            r, g, b = (px >> 11) & 0x1F, (px >> 6) & 0x1F, (px >> 1) & 0x1F
            # 5 bits to 8, replicating the high bits so full-scale stays full-scale
            row += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2)))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path, width, height, raw):
    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--output", default="frames", help="output directory")
    ap.add_argument("--hashes", action="store_true",
                    help="print one hash per frame instead of writing PNGs")
    ap.add_argument("--vi", action="store_true",
                    help="keep only the even rows, which is all the VI scans out")
    args = ap.parse_args()

    frames = extract(args.log)
    if not frames:
        print("no framebuffer dump found in %s" % args.log, file=sys.stderr)
        return 1

    if args.vi:
        for fr in frames:
            if fr.scale != 1:
                # Decimating an already-decimated dump answers a question nobody asked. Refuse
                # rather than produce a plausible picture of nothing in particular.
                print("--vi needs a full-resolution dump; this one is scale=%d "
                      "(build with FBSCALE=1)" % fr.scale, file=sys.stderr)
                return 1
            stride = fr.width * 2
            fr.data = bytearray(b"".join(bytes(fr.data[y * stride:(y + 1) * stride])
                                         for y in range(0, fr.height, 2)))
            fr.height = (fr.height + 1) // 2

    if args.hashes:
        # Hash the raw RGBA5551, not the PNG: zlib output can vary between Python builds and
        # would make the regression diff report changes that are not in the picture.
        for i, fr in enumerate(frames):
            print("%02d %s %dx%d" % (i, hashlib.sha256(bytes(fr.data)).hexdigest()[:16],
                                     fr.width, fr.height))
        return 0

    os.makedirs(args.output, exist_ok=True)
    for i, fr in enumerate(frames):
        path = os.path.join(args.output, "frame%02d.png" % i)
        write_png(path, fr.width, fr.height, to_rgb_rows(fr))
        print("%s  %dx%d (scale %d)" % (path, fr.width, fr.height, fr.scale))
    return 0


if __name__ == "__main__":
    sys.exit(main())
