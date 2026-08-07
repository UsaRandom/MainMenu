#!/usr/bin/env python3
"""
Build an SD tree of invented games with original box art, for screenshots and video.

This is NOT tools/mkfixture.py and must not be confused with it. They pull in opposite
directions on purpose:

  mkfixture  harvests real game codes and titles out of rom_info.c so a scan exercises the
             real 450-entry database. Its art is a flat gradient with the code stamped across
             it, because a tile showing the wrong art must be obvious in a contact sheet.
             It is for finding bugs, and it looks like a test fixture.

  mkdemo     invents every title, every game code and every cheat, and draws box art that is
             meant to look good. Nothing here matches any real game, because the output is
             published in the README and a screenshot full of other people's game names and
             other people's cover scans is not something this repo can ship. It is for
             showing the menu, and it deliberately exercises the database's MISS path.

Everything is derived from the title string, so two runs produce byte-identical output.

  tools/mkdemo.py -o build/demo
  make DEMO=1 FIXTURE=1 sc64 -j8

The art is drawn with Pillow and assets/fonts/Firple-Bold.ttf, the font the menu itself uses.
mkfixture deliberately has no dependencies because it runs in the regression path; this one
runs by hand when someone wants pictures, so a dependency is affordable and the quality is
worth it.

Titles show as the FILENAME, not the header title. index_n64() prefers a curated metadata name,
then the 20-byte header title, then the filename -- and the header title is a fixed-width
uppercase field, so a stub carrying one renders as AURORA DRIFT. The stubs here leave it blank
so the mixed-case filename wins.
"""

import argparse
import math
import os
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

# The ROM header layout is the one thing here that must not drift from the fixture's, so it is
# imported rather than reimplemented. build_header uppercases the title, which is exactly why
# every call below passes an empty one.
from mkfixture import build_header, check_code_for, emit, harvest_games, make_ipl3  # noqa: E402

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("mkdemo.py needs Pillow (pip install pillow); tools/mkfixture.py does not")

# The cache tile, and the only size the menu ever holds art at: the grid column the shape earns
# (src/ui/theme.h) by the height of the system's box (src/library/boxart.h). screen_detail.c blits
# that same cached tile at scale 2, so there is nothing above this for a larger source to serve.
#
# Emitting at the target means the menu decodes exactly these pixels and does no downscale at
# all, so what reaches the screen is what Lanczos produced here rather than what the runtime box
# filter made of it afterwards. A card drawn to be looked at should not go through a resampler
# twice.
#
# All three shapes appear, and which system gets which is the point rather than an accident. The
# menu reads a tile's shape off its own art -- that is the whole mechanism -- and a demo tree
# where every cover is the same shape cannot show it. A real card genuinely is mixed, because the
# art on it comes from wherever its owner got it.
#
# N64 and Master System are LANDSCAPE, and that is not a guess about boxes. The one real corpus
# this project has measured is the SD card it is developed against: 28 covers, every single one
# between 1.369 and 1.469, most of them 256 x 187. Scraper output is title cards and screenshots
# far more often than it is box scans, so the tab the README leads with shows what a real card
# actually looks like. NES and SNES stay portrait, which is what a box scan gives you, and the
# handhelds stay square.
#
# The scenes are all drawn against img.size, so none of them had to change for any of this.
PORTRAIT_SIZE = (109, 155)          # a box scan -- 127 x 181 mm, the narrow column
SQUARE_SIZE = (140, 140)            # Game Boy and Game Boy Color, 126 x 126 mm -- the wide column
LANDSCAPE_SIZE = (140, 98)          # a title card or a screenshot -- also the wide column

ART_SHAPE = {
    "n64": LANDSCAPE_SIZE, "sms": LANDSCAPE_SIZE,
    "nes": PORTRAIT_SIZE,  "snes": PORTRAIT_SIZE,
    "gb": SQUARE_SIZE,     "gbc": SQUARE_SIZE,
}

def art_size(system):
    """The shape this system's covers were scraped at. See ART_SHAPE."""
    return ART_SHAPE[system]

# Drawn at 6x and reduced once with Lanczos. Circles and the diagonals in the perspective
# scenes alias badly at 140 px, and the menu's own quantiser then spends palette entries on
# stair-steps instead of on the picture. The supersampled canvas is 840x588 either way, so
# every scene below draws exactly the pixels it drew when the target was 280x196.
SS = 6

SAVE_TYPES = {                      # rom_save_type_t, for the per-ROM .ini override
    "none": 0, "eeprom4k": 1, "eeprom16k": 2, "sram": 3,
    "sram_banked": 4, "sram1m": 5, "flashram": 6,
}

# The invented studio names lived here, and the per-title column that indexed them is gone from
# the tables below with them. Both went with the lettering they were printed in -- see draw_card().

# Palettes, hand-picked rather than generated. A hue chosen at random from a seed produces
# muddy neighbours about a third of the time, and a grid of box art is judged on the whole
# screen rather than on any one tile.
PALETTES = [
    # (sky top, sky bottom, feature, silhouette near, silhouette far, accent)
    ((0x2B, 0x1B, 0x54), (0xE8, 0x5D, 0x4E), (0xFF, 0xC7, 0x6B), (0x1A, 0x0F, 0x30), (0x3D, 0x22, 0x52), (0xFF, 0xE8, 0xB0)),
    ((0x06, 0x1A, 0x2E), (0x0E, 0x5A, 0x6B), (0x7F, 0xE3, 0xD4), (0x03, 0x10, 0x1C), (0x0A, 0x2E, 0x40), (0xE6, 0xFF, 0xF7)),
    ((0x1B, 0x06, 0x1F), (0x7A, 0x14, 0x50), (0xFF, 0x71, 0xA8), (0x12, 0x03, 0x16), (0x36, 0x0B, 0x33), (0xFF, 0xD9, 0xEC)),
    ((0x0A, 0x1F, 0x12), (0x3E, 0x7A, 0x38), (0xF2, 0xE7, 0x7A), (0x06, 0x12, 0x0B), (0x16, 0x38, 0x1E), (0xEC, 0xFF, 0xD6)),
    ((0x23, 0x18, 0x0B), (0xC9, 0x72, 0x2E), (0xFF, 0xE1, 0x9C), (0x17, 0x0E, 0x06), (0x42, 0x28, 0x11), (0xFF, 0xF3, 0xDD)),
    ((0x10, 0x12, 0x2E), (0x36, 0x3F, 0x8C), (0x9F, 0xB8, 0xFF), (0x08, 0x09, 0x18), (0x1B, 0x20, 0x4C), (0xE9, 0xEE, 0xFF)),
    ((0x26, 0x08, 0x08), (0xA8, 0x2B, 0x1E), (0xFF, 0x9E, 0x4F), (0x18, 0x04, 0x04), (0x4A, 0x11, 0x0D), (0xFF, 0xE2, 0xC4)),
    ((0x05, 0x14, 0x1A), (0x14, 0x4E, 0x52), (0xC8, 0xF0, 0x5C), (0x03, 0x0C, 0x10), (0x0B, 0x2B, 0x30), (0xEF, 0xFF, 0xCE)),
]

# (title, scene, palette, save type). The scene mix is deliberate: three of the eight scenes are
# horizon-based and would look samey if adjacent titles shared one, and the grid sorts by title,
# so the scene column below is ordered against the alphabet, not with it.
N64_GAMES = [
    ("Aurora Drift",        "grid",    5, "eeprom4k"),
    ("Basalt Rally",        "sunset",  4, "sram"),
    ("Cinder Vale",         "forest",  6, "flashram"),
    ("Dune Skipper",        "sunset",  0, "eeprom4k"),
    ("Ember Circuit",       "iso",     6, "eeprom16k"),
    ("Frostline",           "waves",   1, "sram"),
    ("Glass Meridian",      "space",   5, "flashram"),
    ("Harbour Watch",       "city",    1, "eeprom4k"),
    ("Ion Ballet",          "shapes",  2, "none"),
    ("Jade Locomotive",     "forest",  3, "sram"),
    ("Kelp Forest Blues",   "waves",   7, "eeprom4k"),
    ("Lantern Reef",        "space",   1, "eeprom16k"),
    ("Moth and Marrow",     "forest",  2, "flashram"),
    ("Nightbus",            "city",    5, "eeprom4k"),
    ("Obsidian League",     "iso",     0, "sram"),
    ("Paper Sparrow",       "shapes",  3, "none"),
    ("Quarry Kings",        "iso",     4, "eeprom16k"),
    ("Rust Pilgrim",        "sunset",  6, "flashram"),
    ("Solar Tide",          "grid",    2, "eeprom4k"),
    ("Thistledown",         "forest",  7, "none"),
    ("Umber Gate",          "shapes",  4, "sram"),
    ("Vellum Sky",          "waves",   0, "eeprom4k"),
    ("Wax Museum Heist",    "city",    3, "flashram"),
    ("Zinc Garden",         "shapes",  7, "eeprom16k"),
]

EMU_GAMES = [
    ("nes",  ".nes", [("Bit Marsh",      "iso",    3),
                      ("Copper Kite",    "sunset", 4),
                      ("Turnip Knight",  "forest", 3)]),
    ("snes", ".sfc", [("Chrome Lark",    "space",  5),
                      ("Pocket Aurora",  "waves",  1),
                      ("Salt Flats",     "grid",   0)]),
    ("gb",   ".gb",  [("Tin Whistle",    "shapes", 7)]),
    ("gbc",  ".gbc", [("Neon Sparrow",   "city",   2)]),
    ("sms",  ".sms", [("Static Ridge",   "sunset", 6)]),
]

# Invented cheats for invented games. The addresses and values are arbitrary; nothing here can
# match a real game because nothing here IS a real game. Types are restricted to what
# src/boot/cheats.c actually emits -- 80/81 writes and a D0 conditional guarding one -- so
# mkcheatdb.py's filter keeps every group and the demo shows a full list rather than a filtered
# one. See docs/AUDIT.md 2.2 on why a conditional and its write are one group.
DEMO_CHEATS = {
    "Aurora Drift": [
        ("Infinite Boost",          ["80102A44 0063"]),
        ("All Circuits Unlocked",   ["81104C10 FFFF", "81104C12 FFFF"]),
        ("Never Lose Grip",         ["D0102B01 0001", "80102B08 0000"]),
        ("Start On Lap 3",          ["80102A80 0003"]),
        ("Mirror Mode Always On",   ["80104D00 0001"]),
    ],
    "Ember Circuit": [
        ("Infinite Charge",         ["8011A0C4 00FF"]),
        ("Unlock Every Chassis",    ["8111A1E0 FFFF"]),
        ("No Overheat",             ["D011A0F0 0001", "8011A0F4 0000"]),
    ],
    "Glass Meridian": [
        ("Infinite Oxygen",         ["80123C10 0064"]),
        ("All Charts Revealed",     ["81123D00 FFFF", "81123D02 FFFF"]),
        ("Walk Through Bulkheads",  ["80123E44 0001"]),
        ("Max Salvage",             ["81123F00 270F"]),
    ],
    "Nightbus": [
        ("Infinite Fare",           ["8013B208 03E7"]),
        ("Every Stop Unlocked",     ["8113B300 FFFF"]),
        ("Freeze The Timer",        ["D013B40C 0001", "8013B410 0000"]),
    ],
    "Quarry Kings": [
        ("Infinite Dynamite",       ["80145A0C 0063"]),
        ("All Seams Mapped",        ["81145B20 FFFF"]),
    ],
    "Solar Tide": [
        ("Infinite Sail",           ["8015C110 00FF"]),
        ("All Ports Open",          ["8115C200 FFFF", "8115C202 FFFF"]),
        ("Calm Seas",               ["D015C304 0001", "8015C308 0000"]),
        ("Max Cargo",               ["8115C400 270F"]),
    ],
}


# --------------------------------------------------------------------------- helpers

# The two neutrals in src/menu/profile.c's SWATCH, by index. Named here rather than written as 7
# and 8 because the palette lost a duplicate white and shifted once already -- see AUDIT 1ah.
PROFILE_BLACK = 7
PROFILE_WHITE = 8


def seed_of(title):
    return zlib.crc32(title.encode()) & 0xFFFFFFFF


class Rng:
    """A tiny reproducible generator. random.Random would do, but its stream is a promise
    CPython does not make across versions, and this output is committed as PNGs."""

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF or 0x1234567

    def next(self):
        self.s ^= (self.s << 13) & 0xFFFFFFFF
        self.s ^= self.s >> 17
        self.s ^= (self.s << 5) & 0xFFFFFFFF
        return self.s

    def frange(self, lo=0.0, hi=1.0):
        return lo + (hi - lo) * (self.next() / 0xFFFFFFFF)

    def irange(self, lo, hi):
        return lo + self.next() % (hi - lo + 1)


def mix(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def shade(c, t):
    """Toward black for t<1, toward white for t>1."""
    return mix((0, 0, 0), c, t) if t <= 1.0 else mix(c, (255, 255, 255), t - 1.0)


def vgradient(img, top, bottom, y0=0, y1=None):
    d = ImageDraw.Draw(img)
    y1 = img.height if y1 is None else y1
    for y in range(y0, y1):
        d.line([(0, y), (img.width, y)], fill=mix(top, bottom, (y - y0) / max(1, y1 - y0 - 1)))


def game_code_for(title, taken):
    """A four-character code that is NOT in ares' database.

    A demo ROM that collides with a real entry would pick up that game's save type and feature
    mask, and the detail sheet would then show accessories for a game that does not exist --
    which is a lie in a screenshot rather than a bug in the menu.
    """
    letters = [c for c in title.upper() if c.isalpha()]
    for tail in "EPJ":
        for i in range(len(letters) - 1):
            code = "N" + letters[i] + letters[i + 1] + tail
            # NxDx is the homebrew marker index_n64() looks for; steer clear of it.
            if code not in taken and code[2] != "D":
                return code
    sys.exit("could not find a free game code for %r" % title)


# --------------------------------------------------------------------------- scenes

def scene_sunset(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, _accent = pal
    W, H = img.size
    vgradient(img, sky_t, sky_b, 0, int(H * 0.72))

    # A sun cut by horizontal bands. The bands widen toward the bottom, which is the whole
    # trick that makes a flat disc read as light rather than as a circle.
    cx, cy, r = int(W * 0.62), int(H * 0.50), int(H * 0.30)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=feature)
    band, y = 3, cy - int(r * 0.15)
    while y < cy + r:
        d.rectangle([cx - r - 2, y, cx + r + 2, y + band], fill=mix(sky_b, feature, 0.25))
        y += band + max(3, band)
        band += 2

    d.rectangle([0, int(H * 0.72), W, H], fill=far)
    for layer, (col, amp, base) in enumerate([(far, 0.10, 0.72), (near, 0.16, 0.80)]):
        pts = [(0, H)]
        px = 0
        yb = H * base
        while px <= W:
            pts.append((px, yb - abs(math.sin(px / (W * rng.frange(0.10, 0.18)) + layer)) * H * amp))
            px += 4
        pts.append((W, H))
        d.polygon(pts, fill=col)


def scene_space(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    vgradient(img, shade(sky_t, 0.55), sky_b, 0, H)

    # A nebula band, so the three space titles are not three of the same black sky. Drawn as
    # translucent ellipses along a tilted axis rather than as noise, which at 140 px wide would
    # dither into a flat wash and cost palette entries for nothing.
    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(over)
    ax, ay = rng.frange(0.1, 0.5), rng.frange(0.15, 0.55)
    tilt = rng.frange(-0.35, 0.35)
    for i in range(9):
        t = i / 8.0
        cx = W * (ax + t * 0.55)
        cy = H * (ay + tilt * t)
        rx, ry = W * rng.frange(0.10, 0.22), H * rng.frange(0.06, 0.15)
        od.ellipse([cx - rx, cy - ry, cx + rx, cy + ry],
                   fill=mix(sky_b, feature, rng.frange(0.3, 0.8)) + (34,))
    img.paste(Image.alpha_composite(img.convert("RGBA"), over).convert("RGB"), (0, 0))
    d = ImageDraw.Draw(img)

    for _ in range(rng.irange(120, 220)):
        x, y = rng.irange(0, W - 1), rng.irange(0, H - 1)
        s = rng.irange(1, SS + 1)
        d.ellipse([x, y, x + s, y + s], fill=mix(sky_b, accent, rng.frange(0.4, 1.0)))

    cx = int(W * rng.frange(0.52, 0.76))
    cy = int(H * rng.frange(0.28, 0.46))
    r = int(H * rng.frange(0.20, 0.32))
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=feature)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=None, outline=shade(feature, 1.3), width=SS)
    # Terminator: the same disc offset and drawn in shadow, which is cheaper and steadier than
    # shading per pixel and gives a hard crescent that survives the 140 px downscale.
    lit = rng.frange(0.40, 0.70)
    d.ellipse([cx - r - int(r * lit), cy - r - int(r * 0.18),
               cx + r - int(r * lit), cy + r - int(r * 0.18)], fill=shade(feature, 0.45))

    if rng.next() % 3:                                  # two planets in three wear rings
        squash = rng.frange(0.22, 0.40)
        for i in range(rng.irange(2, 4)):
            rr = int(r * (1.40 + i * 0.17))
            d.arc([cx - rr, cy - int(rr * squash), cx + rr, cy + int(rr * squash)],
                  200, 340, fill=mix(sky_b, accent, 0.75 - i * 0.16), width=SS * 2)
    else:                                               # or a moon instead
        mr = int(r * 0.22)
        mx, my = cx - int(r * 1.7), cy + int(r * rng.frange(-0.6, 0.9))
        d.ellipse([mx - mr, my - mr, mx + mr, my + mr], fill=shade(accent, 0.85))

    sy = int(H * rng.frange(0.66, 0.80))
    sx = int(W * rng.frange(0.12, 0.26))
    d.polygon([(sx, sy), (sx + int(W * 0.11), sy + int(H * 0.02)), (sx, sy + int(H * 0.04))],
              fill=accent)
    d.rectangle([sx - int(W * 0.10), sy + int(H * 0.016), sx + int(W * 0.01), sy + int(H * 0.024)],
                fill=near)


def scene_grid(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    horizon = int(H * rng.frange(0.40, 0.52))
    vx = int(W * rng.frange(0.38, 0.62))            # vanishing point, not always centred
    vgradient(img, sky_t, sky_b, 0, horizon)

    r = int(H * rng.frange(0.18, 0.28))
    d.ellipse([vx - r, horizon - r, vx + r, horizon + r], fill=feature)

    # Ridge line along the horizon on some cards, so the sun has something to sit behind.
    if rng.next() % 2:
        pts = [(0, horizon)]
        x = 0
        while x <= W:
            pts.append((x, horizon - abs(math.sin(x / (W * 0.13) + 1.7)) * H * 0.09))
            x += 6
        pts += [(W, horizon)]
        d.polygon(pts, fill=shade(far, 0.85))

    floor = shade(far, 0.7)
    d.rectangle([0, horizon, W, H], fill=floor)

    # Rows spaced by a square law, which is what makes a flat grid read as receding.
    rows = rng.irange(12, 18)
    for i in range(1, rows + 1):
        y = horizon + int((H - horizon) * (i / float(rows)) ** 2.1)
        d.line([(0, y), (W, y)], fill=mix(floor, accent, 1.0 - i / float(rows + 1)), width=SS)
    for i in range(-10, 11):
        d.line([(vx + i * int(W * 0.032), horizon), (vx + i * int(W * 0.42), H)],
               fill=mix(floor, accent, 0.55), width=SS)
    d.line([(0, horizon), (W, horizon)], fill=accent, width=SS * 2)


def scene_forest(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, _accent = pal
    W, H = img.size
    vgradient(img, sky_t, sky_b, 0, H)

    r = int(H * rng.frange(0.10, 0.16))
    mx, my = int(W * rng.frange(0.14, 0.80)), int(H * rng.frange(0.14, 0.30))
    d.ellipse([mx - r, my - r, mx + r, my + r], fill=feature)

    layers = rng.irange(3, 5)
    for layer in range(layers):
        t = layer / float(layers - 1)
        col = mix(far, near, t)
        base = H * (0.50 + t * 0.18)
        # Wider steps further away. A constant step made every ridge the same comb, and the
        # far ones then aliased into a grey band at 140 px.
        step = int(W * (0.055 + t * 0.075))
        pts = [(0, H)]
        x = -step
        while x < W + step:
            top = base - H * rng.frange(0.05, 0.15) * (0.6 + t * 0.7)
            pts += [(x, base), (x + step // 2, top), (x + step, base)]
            x += step
        pts.append((W, H))
        d.polygon(pts, fill=col)


def scene_waves(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    vgradient(img, sky_t, sky_b, 0, H)

    phase = rng.frange(0, 6.28)
    r = int(H * rng.frange(0.09, 0.15))
    mx, my = int(W * rng.frange(0.16, 0.84)), int(H * rng.frange(0.10, 0.22))
    d.ellipse([mx - r, my - r, mx + r, my + r], fill=shade(feature, 1.15))

    for i in range(11):
        t = i / 10.0
        col = mix(feature, near, t) if i % 2 == 0 else mix(accent, far, t)
        y0 = H * (0.16 + t * 0.80)
        pts = []
        x = 0
        while x <= W:
            pts.append((x, y0 + math.sin(x / (W / 3.4) + phase + i * 0.55) * H * 0.055))
            x += 4
        pts += [(W, H), (0, H)]
        d.polygon(pts, fill=col)


def scene_shapes(img, d, pal, rng):
    """A composed poster, not a scatter.

    The first version dropped seven random translucent forms at random positions. It produced
    exactly what that describes: shapes sliced by the frame edge, no focal point, and washed-out
    overlaps. Placing one hero disc first and hanging everything else off it is the only change,
    and it is the difference between a placeholder and something worth putting in a README.
    """
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    vgradient(img, shade(sky_t, 0.8), sky_b, 0, H)

    hx = int(W * rng.frange(0.44, 0.66))
    hy = int(H * rng.frange(0.34, 0.46))
    hr = int(H * rng.frange(0.28, 0.36))

    over = Image.new("RGBA", img.size, (0, 0, 0, 0))
    od = ImageDraw.Draw(over)

    # Concentric arcs behind the disc. The opening rotates per card, which is most of what
    # keeps five shapes cards from reading as one design in five palettes.
    start = rng.irange(-210, -60)
    for i in range(rng.irange(3, 5)):
        rr = int(hr * (1.35 + i * 0.28))
        od.arc([hx - rr, hy - rr, hx + rr, hy + rr], start, start + rng.irange(150, 230),
               fill=mix(sky_b, accent, 0.55 - i * 0.09) + (255,), width=SS * 2)

    od.ellipse([hx - hr, hy - hr, hx + hr, hy + hr], fill=feature + (255,))

    # A triangle anchored on the disc, so the two always intersect.
    ts = int(hr * rng.frange(0.85, 1.25))
    tx = hx + int(hr * rng.frange(-1.1, -0.4))
    ty = hy + int(hr * rng.frange(0.1, 0.7))
    od.polygon([(tx, ty - ts), (tx + ts, ty + ts), (tx - ts, ty + ts)], fill=accent + (170,))

    # A bar through the disc: level on some cards, raked on others.
    by = hy + int(hr * rng.frange(-0.5, 0.5))
    bh = int(H * rng.frange(0.05, 0.10))
    rake = int(H * rng.frange(-0.12, 0.12))
    od.polygon([(int(W * 0.04), by), (int(W * 0.96), by + rake),
                (int(W * 0.96), by + rake + bh), (int(W * 0.04), by + bh)],
               fill=near + (165,))

    # One small solid counterweight, opposite the hero, to break the symmetry.
    sr = int(hr * 0.26)
    sx = hx + int(hr * rng.frange(1.3, 1.9))
    sy = hy + int(hr * rng.frange(-1.0, -0.4))
    od.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=shade(accent, 1.2) + (255,))

    img.paste(Image.alpha_composite(img.convert("RGBA"), over).convert("RGB"), (0, 0))


def scene_iso(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    vgradient(img, sky_t, sky_b, 0, H)

    tw, th = int(W * rng.frange(0.095, 0.125)), 0
    th = tw // 2
    ox, oy = int(W * rng.frange(0.42, 0.58)), int(H * rng.frange(0.28, 0.40))

    def cube(gx, gy, gz, col):
        cx = ox + (gx - gy) * tw
        cy = oy + (gx + gy) * th - gz * th * 2
        d.polygon([(cx, cy - th), (cx + tw, cy), (cx, cy + th), (cx - tw, cy)],
                  fill=shade(col, 1.25))
        d.polygon([(cx - tw, cy), (cx, cy + th), (cx, cy + th + th * 2), (cx - tw, cy + th * 2)],
                  fill=shade(col, 0.78))
        d.polygon([(cx + tw, cy), (cx, cy + th), (cx, cy + th + th * 2), (cx + tw, cy + th * 2)],
                  fill=shade(col, 0.52))

    # Three profiles rather than one. With a single height rule every iso card came out the
    # same shape in a different colour, which on a grid of box art reads as a rendering bug.
    profile = rng.next() % 3
    peak = rng.irange(3, 5)

    def height(gx, gy):
        dist = abs(gx) + abs(gy)
        if profile == 0:                      # ziggurat: tallest in the middle
            return max(1, peak - dist)
        if profile == 1:                      # terrace: a slope across the plate
            return max(1, 1 + (gx + gy + 4) * peak // 8)
        return max(1, 1 + (dist * 7 + gx * 3) % peak)     # broken ground

    # Painter's order: far to near, so a nearer cube always overdraws the one behind it.
    stack = []
    for gy in range(-2, 3):
        for gx in range(-2, 3):
            for gz in range(height(gx, gy)):
                stack.append((gx, gy, gz))
    stack.sort(key=lambda c: (c[0] + c[1], c[2]))
    for gx, gy, gz in stack:
        cube(gx, gy, gz, mix(near, feature, min(1.0, gz / float(peak))) if gz else far)


def scene_city(img, d, pal, rng):
    sky_t, sky_b, feature, near, far, accent = pal
    W, H = img.size
    vgradient(img, sky_t, sky_b, 0, H)

    r = int(H * 0.16)
    d.ellipse([int(W * 0.72) - r, int(H * 0.22) - r, int(W * 0.72) + r, int(H * 0.22) + r],
              fill=feature)

    # Buildings never overlap within a layer -- x advances past each one -- so windows can be
    # drawn with their own building rather than in a second pass over the whole skyline.
    for layer, col in enumerate([far, near]):
        x = -int(W * 0.05)
        while x < W:
            bw = rng.irange(int(W * 0.06), int(W * 0.14))
            top = H - rng.frange(0.18, 0.46) * H * (0.8 + layer * 0.5)
            d.rectangle([x, top, x + bw, H], fill=col)
            if layer == 1:
                lit = mix(near, accent, 0.75)
                for wy in range(int(top + H * 0.035), H - int(H * 0.03), int(H * 0.055)):
                    for wx in range(int(x + W * 0.014), int(x + bw - W * 0.026), int(W * 0.028)):
                        if rng.next() % 5:      # roughly one window in five is dark
                            d.rectangle([wx, wy, wx + int(W * 0.013), wy + int(H * 0.024)],
                                        fill=lit)
            x += bw + rng.irange(2, int(W * 0.02))


SCENES = {
    "sunset": scene_sunset, "space": scene_space, "grid": scene_grid, "forest": scene_forest,
    "waves": scene_waves, "shapes": scene_shapes, "iso": scene_iso, "city": scene_city,
}


# --------------------------------------------------------------------------- card

def draw_card(title, scene, palette, size=None):
    """A cover: one procedural scene, and nothing written on it.

    It used to carry the game's title across the bottom, an invented studio name above that on a
    rule, and a system badge in a pill top right -- and under all of it a scrim, a black gradient
    rising from mid-height, which existed for exactly one reason: so the title stayed readable
    over a lit window on the city scenes.

    All of it is gone. The lettering was set in the menu's own UI font at tile size, so a grid of
    these read as a screenshot of a screenshot -- the covers were captioned in the same typeface
    as the footer under them. Every place the title needs to be seen, the menu already draws it:
    under the selected tile, on the detail sheet, and across the plate a record with no art gets.
    Dropping the scrim with the text is not a separate decision, it is the same one; it was
    darkening the bottom third of every picture in order to be a background for words.

    @p title still seeds the RNG, so a given game draws the same picture it always did.
    """
    aw, ah = size if size else PORTRAIT_SIZE
    W, H = aw * SS, ah * SS
    pal = PALETTES[palette % len(PALETTES)]
    rng = Rng(seed_of(title))

    img = Image.new("RGB", (W, H), pal[0])
    d = ImageDraw.Draw(img)
    SCENES[scene](img, d, pal, rng)

    return img.resize((aw, ah), Image.LANCZOS)


# --------------------------------------------------------------------------- cheats

def write_cheat_corpus(build_dir, codes):
    """Emit .cht files and a key table, then let tools/mkcheatdb.py build the database.

    The database is produced by the real converter rather than written here, so what the demo
    shows is the same file format, the same group filter and the same merge that a real corpus
    goes through. A hand-rolled writer would be a second implementation to keep in step.
    """
    cht_dir = os.path.join(build_dir, "demo-cht")
    os.makedirs(cht_dir, exist_ok=True)
    keys = []

    for title, groups in DEMO_CHEATS.items():
        lines = []
        for i, (name, codes_) in enumerate(groups):
            lines.append('cheat%d_desc = "%s"' % (i, name))
            lines.append('cheat%d_code = "%s"' % (i, "+".join(codes_)))
        lines.insert(0, "cheats = %d" % len(groups))
        with open(os.path.join(cht_dir, title + ".cht"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        keys.append("%s\t%s\t0\t%016X" % (title, codes[title], check_code_for(codes[title])))

    key_path = os.path.join(build_dir, "demo-keys.tsv")
    with open(key_path, "w", encoding="utf-8") as f:
        f.write("# generated by tools/mkdemo.py -- invented games, invented keys\n")
        f.write("\n".join(keys) + "\n")
    return cht_dir, key_path


# --------------------------------------------------------------------------- tree

def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="build/demo")
    ap.add_argument("--build-dir", default="build", help="where the cheat scratch files go")
    ap.add_argument("--no-cheats", action="store_true")
    ap.add_argument("--no-playstate", action="store_true",
                    help="leave out the play history, so Recent and Favourites are empty and "
                         "hidden and the menu opens on N64 -- which is what a take that never "
                         "changes tab needs, since Recent holds one row and cannot be scrolled")
    args = ap.parse_args()

    root = args.output
    ipl3 = make_ipl3(None)

    # Every real code in ares' database, so an invented one can be checked against it.
    taken = {code for code, _title in harvest_games(os.path.join(REPO, "src", "menu", "rom_info.c"))}

    codes = {}
    for title, _s, _p, _sv in N64_GAMES:
        codes[title] = game_code_for(title, taken)
        taken.add(codes[title])

    for title, scene, pal, save in N64_GAMES:
        # Blank header title on purpose: see the module docstring. The stub is 4 KB because
        # rom_config_load reads exactly one header and a scan never looks past it.
        emit(root, os.path.join("roms", "n64", "%s.z64" % title),
             build_header(codes[title], "", 0, ipl3))
        draw_card(title, scene, pal, art_size("n64")).save(
            os.path.join(root, "roms", "n64", "%s.png" % title))
        # A sidecar .ini so the detail sheet has a save type to show. Without it every invented
        # game reports Automatic, which is correct -- these are not in the database -- but says
        # nothing about the sheet. .ini is not a ROM extension, so the scanner skips the file.
        emit(root, os.path.join("roms", "n64", "%s.ini" % title),
             ("[custom_boot]\nsave_type=%d\n" % SAVE_TYPES[save]).encode())

    n_emu = 0
    for folder, ext, entries in EMU_GAMES:
        for i, (title, scene, pal) in enumerate(entries):
            stub = bytes((j * 13 + 0x20) & 0xFF for j in range(2048))
            # Every other SNES stub carries a 512-byte copier header, matching mkfixture:
            # cart_load.c decides whether to strip one from (size & 0x3FF) == 0x200.
            if folder == "snes" and (i % 2) == 1:
                stub = bytes((j * 7 + 0x40) & 0xFF for j in range(512)) + stub
            emit(root, os.path.join("roms", folder, title + ext), stub)
            draw_card(title, scene, pal, art_size(folder)).save(
                os.path.join(root, "roms", folder, "%s.png" % title))
            n_emu += 1

    emit(root, os.path.join("mainmenu", "config.ini"),
         b"[menu]\ndefault_directory=/roms\npal60_enabled=false\n")
    emit(root, os.path.join("saves", ".gitkeep"), b"")
    for core in ("neon64bu.rom", "lithium64.z64", "gb.v64", "gbc.v64", "smsPlus64.z64"):
        emit(root, os.path.join("mainmenu", "emulators", core),
             bytes((i * 31 + 0x10) & 0xFF for i in range(4096)))

    # A play history, so Recent and Favourites are not empty in a screenshot. This is the only
    # way to reach them under ares: the DFS is read-only, so the menu can never write one, but
    # it reads one perfectly well.
    # N64 titles go in by check code, not by name: playstate_key() uses the header check code
    # whenever there is one and only falls back to hashing the filename when there is not, so a
    # --played "Nightbus.z64" record matches nothing at all.
    def cc(title):
        return "%016X" % check_code_for(codes[title])

    if not args.no_playstate:
        subprocess.check_call([
            sys.executable, os.path.join(HERE, "mkplaystate.py"),
            "-o", os.path.join(root, "mainmenu", "cache", "playstate.dat"),
            "--played-code", cc("Quarry Kings"), "--played", "Chrome Lark.sfc",
            "--played-code", cc("Nightbus"), "--played-code", cc("Aurora Drift"),
            "--favorite-code", cc("Solar Tide"), "--favorite-code", cc("Glass Meridian"),
            "--favorite", "Pocket Aurora.sfc",
        ], stdout=subprocess.DEVNULL)

    # A household, for the same reason as the play history above and with the same caveat: the
    # DFS is read-only under ares so the menu can never write a roster, but it reads one perfectly
    # well. Without it the picker is one card and nine dashed rectangles, which is a screenshot of
    # a feature nobody is using.
    #
    # No `icon` key on purpose. The index would have to name a sprite in whatever pack was built,
    # and a capped or re-excluded corpus would make it point at the wrong drawing or at nothing;
    # profile_load() falls back to icon_starter(), which mkiconmeta.py bakes against the pack that
    # actually shipped. The colours are set, because those are a closed palette that cannot drift.
    #
    # Skipped for --no-playstate, which is the fresh-card tree: a card nobody has used has one
    # nameless player, and that is exactly the state the boot picker is supposed to stay out of.
    if not args.no_playstate:
        who = [("ANA", 0, PROFILE_WHITE), ("BEN", 1, PROFILE_BLACK),
               ("CASS", 4, PROFILE_WHITE), ("DIYA", 2, PROFILE_BLACK)]
        ini = ["[profiles]", "version=2", "count=%d" % len(who), "active=0"]
        for i, (name, plate, ink) in enumerate(who):
            ini += ["[p%d]" % (i + 1), "used=1", "name=%s" % name, "theme=",
                    "colour=%d" % plate, "ink=%d" % ink]
        emit(root, os.path.join("mainmenu", "profiles.ini"),
             ("\n".join(ini) + "\n").encode())

    n_cheats = 0
    if not args.no_cheats:
        cht_dir, key_path = write_cheat_corpus(args.build_dir, codes)
        subprocess.check_call([
            sys.executable, os.path.join(HERE, "mkcheatdb.py"),
            "-i", cht_dir, "--keys", key_path,
            "-o", os.path.join(root, "mainmenu", "cheats.db"),
            "--report", os.path.join(args.build_dir, "demo-cheatdb-report.txt"),
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        n_cheats = sum(len(v) for v in DEMO_CHEATS.values())

    total = sum(os.path.getsize(os.path.join(d, f))
                for d, _, fs in os.walk(root) for f in fs)
    print("demo: %d N64 titles, %d emulated-system titles, %d cards, %d cheats in %d games, "
          "%d players, %.1f KB -> %s"
          % (len(N64_GAMES), n_emu, len(N64_GAMES) + n_emu, n_cheats, len(DEMO_CHEATS),
             0 if args.no_playstate else 4, total / 1024.0, root))
    return 0


if __name__ == "__main__":
    sys.exit(main())
