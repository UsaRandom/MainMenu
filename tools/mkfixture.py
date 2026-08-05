#!/usr/bin/env python3
"""
Build a synthetic SD-card tree so the menu has a library to show under ares.

Under an emulator libcart reports no cartridge, flashcart_init falls to its default arm
and sets storage_prefix to "rom:/", so everything the menu reads through that prefix comes
out of the DFS image instead of a real card. Mirror the SD layout into the DFS and the whole
UI runs on unmodified production code with an emulator standing in for the hardware.

The game codes are not invented. They are harvested from the MATCH_* rows in
src/menu/rom_info.c, which is ares' database, so a fixture ROM exercises the real
find_rom_in_database lookup and yields a real save type and feature mask. A fixture built
from made-up codes would exercise only the miss path and would quietly report that every
game is SAVE_TYPE_AUTOMATIC.

Everything is derived from the game code, so two runs produce byte-identical output and
regression hashes stay stable. No randomness anywhere -- see --seed below, which perturbs
selection only, never rendering.

Box art carries the game code drawn as blocky glyphs rather than a pretty gradient. A tile
showing the wrong art is then obvious in a contact sheet; with noise or a smooth ramp it is
invisible, and a mis-mapped index would read as "working".

The stubs are 4 KB by default because rom_config_load reads exactly sizeof(rom_header_t),
which is 0x40 + IPL3_LENGTH(4032) = 4096 bytes, and nothing during a scan reads past it.
Use --rom-size when testing the load path, which does stream the whole file.

  tools/mkfixture.py -o build/fixture --count 40

Limitation worth knowing: the IPL3 area is filled with a pattern, not a real boot block, so
cic_detect reports an unknown CIC and these stubs will not actually boot a game. They are
for exercising the browsing UI. Pass --ipl3 FILE to inject one.
"""

import argparse
import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
ROM_INFO = os.path.join(REPO, "src", "menu", "rom_info.c")

HEADER_SIZE = 0x40 + 4032          # sizeof(rom_header_t); see src/boot/cic.h IPL3_LENGTH
PI_DOM1_BIG_ENDIAN = 0x80371240    # what fix_rom_header_endianness treats as already correct
CLOCK_RATE = 0x0000000F
BOOT_ADDRESS = 0x80246000

# The title-card asset spec from docs/design/README.md section 7: 280x196, exactly 2x the
# 140x98 tile, so the detail sheet shows it 1:1 and the grid downsamples once, offline.
# Upstream's 158x112 box-art size is not what this project consumes.
BOXART_W, BOXART_H = 280, 196

# Guest ROMs for the emulated-system tabs. cart_load_emulator picks the core by extension.
EMU_SYSTEMS = [
    ("nes",  ".nes", ["Sky Duel", "Cavern Quest", "Turbo Tunnel", "Moon Patrol Zero"]),
    ("snes", ".sfc", ["Chrono Drift", "Star Relic", "Pixel Knights"]),
    ("gb",   ".gb",  ["Pocket Racer", "Tiny Tactics"]),
    ("gbc",  ".gbc", ["Colour Quest"]),
    ("sms",  ".sms", ["Sonic Blast Zero"]),
]


# --------------------------------------------------------------------------- database

def harvest_games(path):
    """Pull (game_code, title) out of the MATCH_ID* rows in rom_info.c.

    MATCH_ID takes a 3-char id and matches any region, so a destination char is appended.
    MATCH_ID_REGION and MATCH_ID_REGION_VERSION already carry all four. The trailing //
    comment is the human title and is the only place a readable name exists.
    """
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            source = f.read()
    except OSError as e:
        sys.exit("cannot read %s: %s" % (path, e))

    pattern = re.compile(
        r'MATCH_(ID|ID_REGION|ID_REGION_VERSION)\s*\(\s*"([A-Z0-9]{3,4})"'
        r'[^)]*\)\s*,\s*//\s*(.+?)\s*$',
        re.MULTILINE)

    games, seen = [], set()
    for kind, code, title in (m.groups() for m in pattern.finditer(source)):
        if kind == "ID" and len(code) == 3:
            code += "E"                      # NTSC-U; MATCH_ID matches any region anyway
        if len(code) != 4 or code in seen:
            continue
        seen.add(code)
        games.append((code, title))

    if not games:
        sys.exit("harvested no games from %s -- did the MATCH_ macros change shape?" % path)
    return games


def sanitize(title):
    """A filename the FAT/DFS layer and the browser's extension split both tolerate.

    The database comments annotate variants in brackets -- "Kirby 64: The Crystal Shards
    [Hoshi no Kirby 64 (J)]". Keeping those pushes every title past the length cap and
    truncates it mid-word, which makes the fixture read as a rendering bug.
    """
    title = re.split(r"\s*[\[(]", title, 1)[0]
    out = "".join(c if (c.isalnum() or c in " -_") else "-" for c in title)
    return re.sub(r"[-\s]+", " ", out).strip()[:40] or "Untitled"


# --------------------------------------------------------------------------- ROM stubs

def check_code_for(code):
    """A stable 64-bit value that is deliberately NOT a database check_code.

    Fixture ROMs must match on game code, not by colliding with a real check_code entry,
    because a collision would silently test the wrong lookup path.
    """
    h = zlib.crc32(code.encode()) & 0xFFFFFFFF
    return (h << 32) | (zlib.crc32(code[::-1].encode()) & 0xFFFFFFFF)


def build_header(code, title, version, ipl3):
    h = bytearray(HEADER_SIZE)
    struct.pack_into(">I", h, 0x00, PI_DOM1_BIG_ENDIAN)
    struct.pack_into(">I", h, 0x04, CLOCK_RATE)
    struct.pack_into(">I", h, 0x08, BOOT_ADDRESS)
    h[0x0E] = 0x14                                    # libultra version
    h[0x0F] = ord("K")                                # libultra revision
    struct.pack_into(">Q", h, 0x10, check_code_for(code))
    h[0x20:0x34] = title.upper().encode("ascii", "replace")[:20].ljust(20, b"\x00")
    h[0x3B:0x3F] = code.encode("ascii")
    h[0x3F] = version
    h[0x40:HEADER_SIZE] = ipl3
    return bytes(h)


def make_ipl3(inject):
    if inject:
        with open(inject, "rb") as f:
            data = f.read()
        if len(data) != 4032:
            sys.exit("--ipl3 must be exactly 4032 bytes, got %d" % len(data))
        return data
    # A recognisable, non-zero pattern: an all-zero IPL3 is indistinguishable from a
    # truncated read, and we want a bad stub to look different from a bad file handle.
    return bytes((i * 7 + 0x11) & 0xFF for i in range(4032))


# --------------------------------------------------------------------------- PNG

def write_png(path, width, height, rgb):
    """Minimal RGB8 PNG. Deliberately not using PIL so the harness has no dependencies."""
    raw = bytearray()
    stride = width * 3
    for y in range(height):
        raw.append(0)                                  # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload
                + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


# A 5x7 font covering the character set game codes actually use. Only these -- a full font
# would be dead weight and codes are [A-Z0-9] by construction.
GLYPHS = {
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11111", "00010", "00100", "00010", "00001", "10001", "01110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "11110", "00001", "00001", "10001", "01110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "01100"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "D": ["11100", "10010", "10001", "10001", "10001", "10010", "11100"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01111"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "J": ["00111", "00010", "00010", "00010", "00010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "11011", "10001"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
}


def boxart_for(code, width, height):
    """A per-code colour field with the game code stamped across it in large glyphs."""
    h = zlib.crc32(code.encode()) & 0xFFFFFFFF
    base = ((h >> 16) & 0x7F, (h >> 8) & 0x7F, h & 0x7F)

    buf = bytearray(width * height * 3)
    for y in range(height):
        ramp = y / max(1, height - 1)
        row = y * width * 3
        for x in range(width):
            wash = x / max(1, width - 1)
            i = row + x * 3
            buf[i + 0] = min(255, int(base[0] + 90 * ramp))
            buf[i + 1] = min(255, int(base[1] + 60 * wash))
            buf[i + 2] = min(255, int(base[2] + 90 * (1.0 - ramp)))

    scale = max(2, min(width // (len(code) * 6 + 2), height // 9))
    text_w = len(code) * 6 * scale - scale
    ox = (width - text_w) // 2
    oy = (height - 7 * scale) // 2

    for ci, ch in enumerate(code.upper()):
        rows = GLYPHS.get(ch)
        if rows is None:
            continue
        for gy, bits in enumerate(rows):
            for gx, bit in enumerate(bits):
                if bit != "1":
                    continue
                for py in range(scale):
                    yy = oy + gy * scale + py
                    if not (0 <= yy < height):
                        continue
                    for px in range(scale):
                        xx = ox + (ci * 6 + gx) * scale + px
                        if 0 <= xx < width:
                            i = (yy * width + xx) * 3
                            buf[i] = buf[i + 1] = buf[i + 2] = 0xF0
    return bytes(buf)


# --------------------------------------------------------------------------- tree

def emit(root, rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", default="build/fixture")
    ap.add_argument("-n", "--count", type=int, default=40,
                    help="number of N64 stubs (default 40)")
    ap.add_argument("--rom-size", default="4K",
                    help="stub size, e.g. 4K or 1M (default 4K -- only the header is scanned)")
    ap.add_argument("--seed", type=int, default=0,
                    help="offset into the harvested list; changes which games, not how they render")
    ap.add_argument("--ipl3", help="4032-byte IPL3 to inject so stubs are bootable")
    ap.add_argument("--no-art", action="store_true", help="skip box art (faster; tests the placeholder path)")
    ap.add_argument("--art-from", metavar="DIR",
                    help="a tree of <GAMECODE>/boxart_front.png to draw real art from where a "
                         "game code matches, falling back to generated art otherwise. The real "
                         "corpus is the one that finds bugs: it is 112-2118 px wide and a quarter "
                         "of it is portrait.")
    ap.add_argument("--rom-info", default=ROM_INFO)
    args = ap.parse_args()

    m = re.fullmatch(r"(\d+)([KM]?)", args.rom_size.upper())
    if not m:
        sys.exit("--rom-size must look like 4K or 1M")
    rom_size = int(m.group(1)) * {"": 1, "K": 1024, "M": 1024 * 1024}[m.group(2)]
    if rom_size < HEADER_SIZE:
        sys.exit("--rom-size must be at least %d bytes (one ROM header)" % HEADER_SIZE)

    games = harvest_games(args.rom_info)
    if args.seed:
        games = games[args.seed % len(games):] + games[:args.seed % len(games)]
    picked = games[:args.count]

    root = args.output
    ipl3 = make_ipl3(args.ipl3)
    n_art = 0
    n_real = 0

    for code, title in picked:
        name = sanitize(title)
        body = build_header(code, name, 0, ipl3)
        if rom_size > HEADER_SIZE:
            # Pad deterministically. Zero padding compresses to nothing in the DFS and would
            # make --rom-size useless for measuring load throughput.
            filler = bytes((i * 31 + 0x5A) & 0xFF for i in range(256))
            body += (filler * ((rom_size - HEADER_SIZE) // 256 + 1))[:rom_size - HEADER_SIZE]
        emit(root, os.path.join("roms", "n64", "%s.z64" % name), body)

        if not args.no_art:
            rel = os.path.join("menu", "metadata", code[0], code[1], code[2], code[3],
                               "boxart_front.png")
            path = os.path.join(root, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)

            real = None
            if args.art_from:
                cand = os.path.join(args.art_from, code[0], code[1], code[2], code[3],
                                    "boxart_front.png")
                if os.path.exists(cand):
                    real = cand
            if real:
                with open(real, "rb") as src, open(path, "wb") as dst:
                    dst.write(src.read())
                n_real += 1
            else:
                write_png(path, BOXART_W, BOXART_H, boxart_for(code, BOXART_W, BOXART_H))
            n_art += 1

    n_emu = 0
    for folder, ext, titles in EMU_SYSTEMS:
        for i_t, t in enumerate(titles):
            stub = bytes((i * 13 + 0x20) & 0xFF for i in range(2048))
            # Every other SNES stub carries a 512-byte copier header. cart_load.c decides whether
            # to strip one from `(size & 0x3FF) == 0x200`, and with every stub a round 2048 bytes
            # that branch had never once been taken -- an untested branch that decides whether the
            # emulated ROM is uploaded aligned or 512 bytes out.
            if folder == "snes" and (i_t % 2) == 1:
                stub = bytes((i * 7 + 0x40) & 0xFF for i in range(512)) + stub
            emit(root, os.path.join("roms", folder, sanitize(t) + ext), stub)
            n_emu += 1

    emit(root, os.path.join("menu", "config.ini"),
         b"[menu]\n"
         b"default_directory=/roms\n"
         b"pal60_enabled=false\n")
    emit(root, os.path.join("saves", ".gitkeep"), b"")

    # Emulator cores, as stubs. Not runnable -- under ares the flashcart is the dummy vtable whose
    # load_rom returns OK without doing anything, so what these test is everything *around* the
    # upload: which core a system maps to, the path built for it, the copier-header decision, the
    # save path, and the lithium64 -> sodium64 fallback. With the directory empty every one of
    # those was unreachable behind an immediate CART_LOAD_ERR_EMU_NOT_FOUND.
    #
    # sodium64.z64 is deliberately absent: with both present the fallback can never run, and the
    # primary is the path that matters. See docs/AUDIT.md 1n.
    for core in ("neon64bu.rom", "lithium64.z64", "gb.v64", "gbc.v64", "smsPlus64.z64"):
        emit(root, os.path.join("menu", "emulators", core),
             bytes((i * 31 + 0x10) & 0xFF for i in range(4096)))

    total = sum(os.path.getsize(os.path.join(d, f))
                for d, _, fs in os.walk(root) for f in fs)
    print("fixture: %d N64 stubs, %d emulated-system stubs, %d box art (%d real), "
          "%.1f KB total -> %s"
          % (len(picked), n_emu, n_art, n_real, total / 1024.0, root))
    print("first five: " + ", ".join("%s %s" % (c, sanitize(t)) for c, t in picked[:5]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
