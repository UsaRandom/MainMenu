#!/usr/bin/env python3
"""
Turn the upstream SC64 logo SVG into the boot plate's 192 x 135 mark.

    tools/mklogo.py --fetch -o assets/images/sc64_logo.png

The logo lives in Polprzewodnikowy/SummerCart64 as `assets/sc64_logo.svg`, 256 x 180 -- exactly
the 1.422 aspect docs/design/README.md section 4.1 asks the boot mark to be, so it goes to
192 x 135 with no cropping and no letterboxing.

There is no SVG rasteriser on this machine beyond macOS Quick Look, which only renders into a
square box and letterboxes to fit. So: render large, find the actual content by scanning for
non-transparent pixels rather than assuming where Quick Look put it, and box-filter down. Trusting
a computed offset would silently shift the mark the day Quick Look changes its padding.

Output is committed, because it is a build input the ROM needs and re-deriving it requires a
rasteriser that is not part of the toolchain.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zlib

SVG_URL = ("https://raw.githubusercontent.com/Polprzewodnikowy/SummerCart64/"
           "refs/heads/main/assets/sc64_logo.svg")
OUT_W, OUT_H = 192, 135


def read_png_rgba(path):
    """(w, h, [[ (r,g,b,a), ... ], ...]) for 8-bit RGB or RGBA PNGs."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"

    pos, idat = 8, bytearray()
    w = h = color = None
    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if ctype == b"IHDR":
            w, h, depth, color, _c, _f, _i = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color not in (2, 6):
                sys.exit("unsupported PNG: depth %d colour %d" % (depth, color))
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break
        pos += 12 + length

    nch = 4 if color == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = w * nch
    rows, prev, p = [], bytearray(stride), 0
    for _y in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        if f == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            sys.exit("unknown PNG filter %d" % f)
        rows.append(line)
        prev = line

    out = []
    for y in range(h):
        r = rows[y]
        out.append([(r[x * nch], r[x * nch + 1], r[x * nch + 2],
                     r[x * nch + 3] if nch == 4 else 255) for x in range(w)])
    return w, h, out


def content_bounds(w, h, px):
    """Tightest box holding anything that is not the background.

    Quick Look renders onto OPAQUE WHITE rather than transparency, so keying off alpha finds the
    whole square and the mark ends up letterboxed inside its own 192x135 box. Background is taken
    from the corner pixel instead -- measured, not assumed, since a future Quick Look could
    equally well pick black.
    """
    bg = px[0][0]

    def is_bg(p):
        return (abs(p[0] - bg[0]) < 12 and abs(p[1] - bg[1]) < 12
                and abs(p[2] - bg[2]) < 12 and p[3] > 8)

    x0, y0, x1, y1 = w, h, -1, -1
    for y in range(h):
        row = px[y]
        for x in range(w):
            if row[x][3] <= 8 or is_bg(row[x]):
                continue
            if x < x0: x0 = x
            if x > x1: x1 = x
            if y < y0: y0 = y
            if y > y1: y1 = y
    if x1 < 0:
        sys.exit("rasterised image has no content distinguishable from its background")
    return x0, y0, x1 + 1, y1 + 1


def box_resize(px, x0, y0, x1, y1, dw, dh):
    """Area-average down to dw x dh, compositing onto black as it goes."""
    sw, sh = x1 - x0, y1 - y0
    out = []
    for dy in range(dh):
        sy0 = y0 + (dy * sh) // dh
        sy1 = max(sy0 + 1, y0 + ((dy + 1) * sh) // dh)
        row = []
        for dx in range(dw):
            sx0 = x0 + (dx * sw) // dw
            sx1 = max(sx0 + 1, x0 + ((dx + 1) * sw) // dw)
            r = g = b = n = 0
            for y in range(sy0, sy1):
                for x in range(sx0, sx1):
                    pr, pg, pb, _pa = px[y][x]
                    # Colours are taken as rendered. The source is already flattened onto an
                    # opaque background by Quick Look, so there is no alpha left to composite and
                    # multiplying by it would only darken the mark.
                    r += pr
                    g += pg
                    b += pb
                    n += 1
            row.append((r // n, g // n, b // n))
        out.append(row)
    return out


def write_png_rgb(path, w, h, rows):
    raw = bytearray()
    for r in rows:
        raw.append(0)
        for (a, b, c) in r:
            raw += bytes((a, b, c))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="assets/images/sc64_logo.png")
    ap.add_argument("--svg", default=None, help="use a local SVG instead of fetching")
    ap.add_argument("--fetch", action="store_true")
    ap.add_argument("--background", default="#000000",
                    help="composite onto this before downscaling; '' to keep the rasteriser's")
    ap.add_argument("--render-size", type=int, default=768,
                    help="Quick Look render size; 4x the target so the downscale has data")
    args = ap.parse_args()

    tmp = tempfile.mkdtemp()
    svg = args.svg
    if svg is None or args.fetch:
        svg = os.path.join(tmp, "sc64_logo.svg")
        print("fetching %s" % SVG_URL, file=sys.stderr)
        urllib.request.urlretrieve(SVG_URL, svg)


    def render(path):
        subprocess.run(["qlmanage", "-t", "-s", str(args.render_size), "-o", tmp, path],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        out = os.path.join(tmp, os.path.basename(path) + ".png")
        if not os.path.exists(out):
            sys.exit("qlmanage produced nothing -- is Quick Look available?")
        return read_png_rgba(out)

    # Rendered twice, and the two renders are used for different things.
    #
    # Quick Look flattens onto WHITE and returns no alpha, so a mark destined for a #000000 plate
    # would arrive with a white card behind it and a white halo on every antialiased edge.
    # Injecting a background rect makes the rasteriser composite properly -- the only way to get
    # the edge pixels right, since recolouring near-white afterwards leaves the halos.
    #
    # But that same rect defeats corner-based content detection: the rect is in the SVG's own
    # 256x180 coordinate space, so Quick Look letterboxes it and the corner ends up white while
    # the mark's own background is black. So geometry is measured on the PLAIN render and applied
    # to the composited one. Both are the same rasterisation at the same size.
    w, h, plain = render(svg)
    x0, y0, x1, y1 = content_bounds(w, h, plain)
    print("rendered %dx%d, content at (%d,%d)-(%d,%d) aspect %.3f"
          % (w, h, x0, y0, x1, y1, (x1 - x0) / (y1 - y0)), file=sys.stderr)

    px = plain
    if args.background:
        src = open(svg, encoding="utf-8").read()
        i = src.index(">", src.index("<svg")) + 1
        src = (src[:i]
               + '<rect x="0" y="0" width="100%%" height="100%%" fill="%s"/>' % args.background
               + src[i:])
        bgsvg = os.path.join(tmp, "bg_" + os.path.basename(svg))
        open(bgsvg, "w", encoding="utf-8").write(src)
        bw, bh, comp = render(bgsvg)
        if (bw, bh) != (w, h):
            sys.exit("the two renders disagree on size (%dx%d vs %dx%d)" % (w, h, bw, bh))
        px = comp

    rows = box_resize(px, x0, y0, x1, y1, OUT_W, OUT_H)
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    write_png_rgb(args.output, OUT_W, OUT_H, rows)
    print("%s  %dx%d  %d bytes" % (args.output, OUT_W, OUT_H, os.path.getsize(args.output)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
