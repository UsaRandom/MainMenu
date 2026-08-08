#!/bin/sh
# Build a release ROM, prove it is one, and put it on the card.
#
#     tools/deploy.sh                 # build, check, copy to /Volumes/SC64, eject
#     tools/deploy.sh --no-eject      # leave the card mounted
#     tools/deploy.sh -d /Volumes/X   # somewhere else
#
# ## Why this exists
#
# tools/regress.sh builds the ROM itself -- `make FIXTURE=1 DEV_HARNESS=1 INPUT_SCRIPT=... sc64`
# at regress.sh:95 -- and leaves the result in output/sc64menu.n64, the same path a release build
# writes to. Run the suite and then copy output/sc64menu.n64 to the card and the card gets a
# harness build: src/dev/hooktest.c executing at boot, the fixture packed instead of the real
# assets, and a compiled-in input script driving the pad.
#
# That is not hypothetical. Three consecutive hardware runs of the cheat investigation were done
# on exactly that ROM, and all three came back "black screen" and were recorded as evidence. What
# caught it was the user noticing green and red screens flashing before the menu appeared -- which
# is hooktest's flash scenarios painting the real display. Three round trips, and the finding of
# each was an artefact of the build. See AUDIT.md 1at.
#
# regress.sh's own docstring already warns that it throws away a hand-built ROM. The warning was
# read and the trap was walked into anyway, which is what makes a check the right answer rather
# than a reminder.
#
# The check is the size and the strings. A harness build is about 4.5 MB against 8.3 MB, because
# FIXTURE packs the synthetic tree instead of the real assets, and it carries hooktest's messages.
# Either one alone would do; both, because this cost three round trips.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

DEST=/Volumes/SC64
EJECT=1

while [ $# -gt 0 ]; do
    case "$1" in
        -d) DEST=$2; shift 2 ;;
        --no-eject) EJECT=0; shift ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

: "${N64_INST:=$HOME/n64inst-preview}"
export N64_INST

ROM=output/sc64menu.n64

# A six-character code over the source tree, shown on the boot plate as `MENU v1.0.0 A1B2C3`.
#
# Deterministic: the same tree always produces the same code, so it is not a timestamp and two
# builds of the same source are still identical. But any change to any source file changes it, and
# that is the point -- four hardware runs have now been explained, or half-explained, by the ROM
# on the card not being the ROM that was built, and until this existed there was no way to ask the
# console which one it was running.
CODE=$(find src Makefile -type f | LC_ALL=C sort | xargs cat | shasum | cut -c1-6 | tr 'a-f' 'A-F')

# Read from the Makefile rather than written here. This said "v0.1.0" for one deploy after the
# Makefile said v1.0.0, so the plate on the card reported a version that had not existed for an
# hour -- which is the exact failure the source code beside it is here to prevent.
VERSION=$(sed -n 's/^MENU_VERSION[[:space:]]*?*=[[:space:]]*"\(.*\)".*$/\1/p' Makefile)
[ -n "$VERSION" ] || { echo "no MENU_VERSION in Makefile" >&2; exit 1; }

echo "== building a release ROM  [$VERSION $CODE]"
# Unconditionally, and with no knobs. Whatever was in output/ before this line is not trusted --
# that is the entire point of the file.
make sc64 -j8 MENU_VERSION="\"$VERSION $CODE\""

echo
echo "== checking it is one"
fail=0

# The ELF, not the ROM. libdragon compresses the ELF into the .z64, so `strings` on the ROM finds
# whatever literal runs happen to survive the compressor and misses the rest -- it reported zero
# HOOKTEST markers in a build that also contained none of screen_launch.c's format strings, which
# were demonstrably compiled in. A check that is right by luck is not a check.
if strings -a build/N64FlashcartMenu.elf | grep -q HOOKTEST; then
    echo "  REFUSING: this is a DEV_HARNESS build -- hooktest is linked in"
    fail=1
else
    echo "  no harness markers in the ELF"
fi

size=$(wc -c < "$ROM" | tr -d ' ')
if [ "$size" -lt 6000000 ]; then
    echo "  REFUSING: $ROM is $size bytes; a release ROM packs the real assets and is ~8.3 MB"
    fail=1
else
    echo "  $size bytes"
fi

[ "$fail" -eq 0 ] || exit 1

if [ ! -d "$DEST" ]; then
    echo
    echo "  $DEST is not mounted -- built and checked, nothing copied"
    exit 1
fi

echo
echo "== what the console left on the card since last deploy"
# The forensic check that kept being forgotten because this script ejects: files the firmware or
# a run may have created. sc64menu.n64.main/.prev appearing UNPROMPTED would prove the M64's boot
# chain manages menu copies itself -- an armed experiment since AUDIT 2i. reprobe.tmp lingering
# means the diagnostic's live probe opened a file and never got to remove it.
for f in sc64menu.n64.main sc64menu.n64.prev mainmenu/cache/reprobe.tmp \
         mainmenu/launch.log mainmenu/romdump-before.bin; do
    [ -e "$DEST/$f" ] && echo "  FOUND: $f ($(wc -c < "$DEST/$f" | tr -d ' ') bytes, $(stat -f %Sm "$DEST/$f"))"
done
echo "  (nothing above this line = none of the watched files exist)"

echo
echo "== copying"
cp "$ROM" "$DEST/sc64menu.n64"
sync

# Unmount, remount, and hash what is actually on the medium. Reading the file back straight after
# a copy can be served out of the page cache, which makes the check agree with the copy rather
# than with the card -- exactly the kind of self-agreement this whole file exists to break.
want=$(shasum -a 256 "$ROM" | cut -d' ' -f1)
diskutil unmount "$DEST" >/dev/null
diskutil mount "$(basename "$DEST")" >/dev/null 2>&1 || diskutil mount "$DEST" >/dev/null 2>&1 || true
if [ -f "$DEST/sc64menu.n64" ]; then
    got=$(shasum -a 256 "$DEST/sc64menu.n64" | cut -d' ' -f1)
    if [ "$want" = "$got" ]; then
        echo "  on the card: $got"
    else
        echo "  MISMATCH after remount: card has $got, built $want"
        exit 1
    fi
else
    echo "  could not remount to verify; card holds $want if the copy succeeded"
fi

echo
echo "  boot plate will read:  MENU $VERSION $CODE"

if [ "$EJECT" -eq 1 ]; then
    diskutil unmount "$DEST" >/dev/null 2>&1 && echo "  ejected"
fi
