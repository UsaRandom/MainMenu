#!/usr/bin/env python3
"""
Mirror a real SD card into a DFS tree small enough to pack into a ROM.

    tools/mksdmirror.py /Volumes/SC64 -o build/sdcard
    make FIXTURE=1 FIXTURE_DIR=build/sdcard sc64 -j8

ares has no SD card, so the only way to run against a real library is to put it in the DFS -- and
a real library does not fit. The card this was written for holds 559 MB across 27 ROMs, one of
which is Conker at 64 MB on its own.

It fits because **almost none of those bytes are ever read by the menu**. `rom_config_load()`
reads one 4 KB header per ROM and nothing else; that is the measurement the whole scan design
rests on. The rest is read only by `flashcart_load_rom()`, and under ares `flashcart_is_dummy()`
is true, so a launch is simulated and never touches the file. Truncating every ROM to its first
8 KB therefore changes nothing the menu can observe short of pressing A, and takes the tree from
559 MB to about 2 MB.

What is copied whole is everything the menu actually reads end to end: the cover art, so the real
JPEG and PNG decode paths run against real files rather than gradients, and `mainmenu/cheats.db`, so
the lookups are the real ones. Emulator cores are truncated with the ROMs -- nothing opens them
under a dummy flashcart.

**This is not a substitute for tools/mkfixture.py in a measurement.** Scan timings taken here are
honest, because the header read is the real one, but anything downstream of a file's full contents
is not. It exists to answer "what does the menu do with MY card", which is the one question the
fixture cannot answer.
"""

import argparse
import os
import shutil
import sys

# Only the header is read at scan time. 8 KB rather than the 4 KB rom_config_load() asks for,
# because a file shorter than the read is a case nothing here has ever exercised and this is not
# the place to find out.
HEADER_BYTES = 8192

TRUNCATE = {".z64", ".n64", ".v64", ".rom", ".smc", ".sfc", ".gb", ".gbc", ".sms", ".gg", ".nes"}

# The menu's own ROM and its RTC sidecar sit at the card root. The scan reaches the root now, so
# the menu does exclude sc64menu.n64 by name -- but copying it would pack a ROM inside a ROM
# regardless, and the mirror is meant to be small.
SKIP_NAMES = {"sc64menu.n64", "sc64menu.rtc"}

# macOS litters removable media. AppleDouble files in particular are named ._<original>, so they
# share the real file's extension and would otherwise be mirrored as if they were ROMs.
SKIP_PREFIXES = ("._",)
SKIP_DIRS = {".Spotlight-V100", ".Trashes", ".fseventsd", ".TemporaryItems"}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="the mounted card, e.g. /Volumes/SC64")
    ap.add_argument("-o", "--output", default="build/sdcard")
    args = ap.parse_args()

    if not os.path.isdir(args.source):
        sys.exit("no such directory: %s" % args.source)

    shutil.rmtree(args.output, ignore_errors=True)

    copied = truncated = 0
    bytes_in = bytes_out = 0

    for root, dirs, files in os.walk(args.source):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(SKIP_PREFIXES)]
        rel = os.path.relpath(root, args.source)
        dest_dir = args.output if rel == "." else os.path.join(args.output, rel)
        os.makedirs(dest_dir, exist_ok=True)

        for name in sorted(files):
            if name in SKIP_NAMES or name.startswith(SKIP_PREFIXES):
                continue
            src = os.path.join(root, name)
            dst = os.path.join(dest_dir, name)
            size = os.path.getsize(src)
            bytes_in += size

            if os.path.splitext(name)[1].lower() in TRUNCATE and size > HEADER_BYTES:
                with open(src, "rb") as fh:
                    head = fh.read(HEADER_BYTES)
                with open(dst, "wb") as fh:
                    fh.write(head)
                truncated += 1
                bytes_out += len(head)
            else:
                shutil.copy2(src, dst)
                copied += 1
                bytes_out += size

    # Empty directories left behind by the skips are noise in the DFS listing.
    for root, dirs, files in os.walk(args.output, topdown=False):
        if not os.listdir(root) and root != args.output:
            os.rmdir(root)

    print("    [SDMIRROR] %s -> %s" % (args.source, args.output))
    print("    [SDMIRROR] %d copied whole, %d truncated to %d bytes"
          % (copied, truncated, HEADER_BYTES))
    print("    [SDMIRROR] %.1f MB -> %.1f MB" % (bytes_in / 1048576.0, bytes_out / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
