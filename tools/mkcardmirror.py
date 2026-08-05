#!/usr/bin/env python3
"""Mirror a real SD card into a fixture tree small enough to pack into the ROM.

ares has no SD card. The fixture goes in through DFS, which caps what can be carried at the size
of the ROM itself -- and a real card is hundreds of megabytes of games. So the interactive harness
has always browsed `tools/mkfixture.py`'s synthetic library, which is the right thing to *measure*
against (it harvests real game codes out of rom_info.c so the database matches) and the wrong
thing to *look* at, because none of the titles are yours.

This is the other half. Everything the menu reads at scan time comes out of the first 4 KB of a
ROM -- `rom_config_load()` does one 4 KB header read per file and nothing else -- so truncating
every game to its header produces a library that is byte-identical from the scanner's point of
view and 99.99% smaller. 49 N64 titles become 200 KB.

What you get: your filenames, your titles, your game codes, your save types, your box art, in the
order your card sorts them. What you do not get, and cannot:

  * **Nothing launches.** A 4 KB stub is a header and no game. The launch screen will start and
    the boot will not survive it. This tree is for looking at the menu, not for playing.
  * **Saves are not copied.** They would only be read back by a game that cannot run here.

Art comes across whole, because it is small (under 500 KB for a card of 50 games) and because a
downscaled copy would make every decode number measured against this tree a lie.

    tools/mkcardmirror.py /Volumes/SC64 -o build/fixture-card
    make FIXTURE=1 FIXTURE_DIR=build/fixture-card sc64
"""

import argparse
import os
import shutil
import sys

# One 4 KB read per ROM is what rom_config_load() does, so that is exactly what a stub has to
# carry. Kept as a named constant rather than a literal because the day the scanner reads more,
# this tree starts lying about titles rather than failing loudly.
HEADER_BYTES = 4096

ROM_EXT = {".z64", ".n64", ".v64", ".smc", ".sfc", ".nes", ".gb", ".gbc", ".sms", ".ndd"}
ART_EXT = {".png", ".jpg", ".jpeg"}

# Directories the menu itself excludes from a scan (library.c SCAN_SKIP), plus saves. Mirroring
# them would copy the whole cheat database and art pack for no benefit -- the ones that matter are
# handled explicitly below.
SKIP_DIRS = {"saves", "System Volume Information", "$RECYCLE.BIN"}


def stub(src, dst):
    """Write the first HEADER_BYTES of src to dst, zero-padded if the source is shorter."""
    with open(src, "rb") as f:
        head = f.read(HEADER_BYTES)
    if len(head) < HEADER_BYTES:
        head += b"\0" * (HEADER_BYTES - len(head))
    with open(dst, "wb") as f:
        f.write(head)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("card", help="the mounted card, e.g. /Volumes/SC64")
    ap.add_argument("-o", "--out", default="build/fixture-card")
    ap.add_argument("--with-cheats", action="store_true",
                    help="copy cheats.db too; 1.5 MB, and the only reason to is to see the badges")
    args = ap.parse_args()

    if not os.path.isdir(args.card):
        sys.exit(f"no card at {args.card}")

    shutil.rmtree(args.out, ignore_errors=True)
    os.makedirs(args.out, exist_ok=True)

    roms = arts = 0
    rom_bytes_real = 0

    for dirpath, dirnames, filenames in os.walk(args.card):
        dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS and not d.startswith(".")]
        rel = os.path.relpath(dirpath, args.card)

        # The menu's own folder is handled below, not walked: it holds the art pack and the cheat
        # database, which are megabytes each and neither is a game.
        if rel.split(os.sep)[0] in ("menu", "mainmenu", "metadata", "emulators"):
            continue

        for name in filenames:
            # AppleDouble files carry the real file's extension, so they would each index as a
            # second copy of a game. The menu's scanner skips them by the leading dot and so does
            # this, or the mirror would hold twice as many titles as the card.
            if name.startswith("."):
                continue
            ext = os.path.splitext(name)[1].lower()
            src = os.path.join(dirpath, name)
            out_dir = os.path.join(args.out, rel) if rel != "." else args.out

            if ext in ROM_EXT:
                os.makedirs(out_dir, exist_ok=True)
                stub(src, os.path.join(out_dir, name))
                rom_bytes_real += os.path.getsize(src)
                roms += 1
            elif ext in ART_EXT:
                os.makedirs(out_dir, exist_ok=True)
                shutil.copy2(src, os.path.join(out_dir, name))
                arts += 1

    if args.with_cheats:
        for folder in ("mainmenu", "menu"):
            src = os.path.join(args.card, folder, "cheats.db")
            if os.path.isfile(src):
                os.makedirs(os.path.join(args.out, "mainmenu"), exist_ok=True)
                shutil.copy2(src, os.path.join(args.out, "mainmenu", "cheats.db"))
                break

    total = sum(os.path.getsize(os.path.join(d, f))
                for d, _, fs in os.walk(args.out) for f in fs)

    print(f"{roms} ROM headers, {arts} art files -> {args.out}")
    print(f"{total / 1024:.0f} KB, from {rom_bytes_real / 1024 / 1024:.0f} MB of real games")
    print("Nothing in this tree can be launched: every ROM is its first 4 KB and no more.")


if __name__ == "__main__":
    main()
