#!/usr/bin/env python3
"""
Generate the JPEG variants tools/hosttest/test_jpeg.c decodes.

Generated rather than committed for the same reason nothing else binary is committed here, and
generated rather than downloaded so the suite has no network dependency. Everything is derived
from a fixed drawing, so two runs produce the same files and the test is stable.

The set is chosen from what a user can produce without trying:

  baseline_420  the default of every encoder; 16x16 MCU
  baseline_444  no chroma subsampling; 8x8 MCU, which is a different path through jpeg_fill_band
  gray          "Save As -> Grayscale"; one component, and the branch that replicates it
  progressive   a checkbox in every editor, and the one thing picojpeg cannot read
  gray_prog     both at once, so the rejection is not accidentally colour-specific
  truncated     a baseline file cut in half, to prove "damaged" and "progressive" stay distinct

Needs Pillow. If it is missing the suite skips this section rather than failing, because the
rest of the host tests have no third-party dependency at all and should not acquire one.
"""

import os
import sys

W, H = 280, 196


def main():
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("Pillow not installed; skipping JPEG fixtures", file=sys.stderr)
        return 2

    out = sys.argv[1] if len(sys.argv) > 1 else "build/hosttest/jpegs"
    os.makedirs(out, exist_ok=True)

    # A gradient with hard-edged shapes on top. The gradient makes chroma subsampling visible and
    # the hard edges give the DC-only path something that is not flat, so a decoder that returns
    # nothing but zeros cannot pass by accident.
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    for y in range(H):
        d.line([(0, y), (W, y)], fill=(int(255 * y / H), 40, 255 - int(255 * y / H)))
    d.rectangle([40, 40, 240, 150], fill=(250, 220, 20))
    d.ellipse([90, 60, 190, 130], fill=(20, 20, 120))

    im.save(os.path.join(out, "baseline_420.jpg"), "JPEG", quality=85, subsampling=2)
    im.save(os.path.join(out, "baseline_444.jpg"), "JPEG", quality=95, subsampling=0)
    im.save(os.path.join(out, "progressive.jpg"), "JPEG", quality=85, progressive=True)
    g = im.convert("L")
    g.save(os.path.join(out, "gray.jpg"), "JPEG", quality=85)
    g.save(os.path.join(out, "gray_prog.jpg"), "JPEG", quality=85, progressive=True)

    # Cut a baseline file in half, keeping the header so it still opens.
    whole = open(os.path.join(out, "baseline_420.jpg"), "rb").read()
    with open(os.path.join(out, "truncated.jpg"), "wb") as f:
        f.write(whole[: len(whole) // 2])

    for name in sorted(os.listdir(out)):
        print("  %-20s %6d B" % (name, os.path.getsize(os.path.join(out, name))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
