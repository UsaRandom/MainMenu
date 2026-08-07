#!/bin/sh
# Build the fixture ROM and launch it in ares, interactively.
#
#     tools/run.sh                              # browse the synthetic library by hand
#     tools/run.sh --sample                     # a full card of box art at realistic shapes
#     tools/run.sh --sample true                # ... with every cover the right shape
#     tools/run.sh --demo                       # the invented library the README is made from
#     tools/run.sh --script browse-roms         # replay a scripted run and watch it
#     tools/run.sh --no-build                   # reuse whatever is in output/
#
# For a headless, hashed run use tools/regress.sh instead.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

ARES="${ARES:-/Volumes/Storage/tools/ares-v148/ares-v148/ares.app/Contents/MacOS/ares}"
SETTINGS="${ARES_SETTINGS:-$HOME/Library/Application Support/ares/settings.bml}"
BUILD=1
SCRIPT=
TREE="FIXTURE=1"

usage() { sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --script) SCRIPT=tools/inputs/$2.txt; shift 2 ;;
        # The mix is optional and must not swallow the next flag: `--sample --no-build` has to
        # mean the realistic mix, not a mix called "--no-build" and a tree that does not exist.
        --sample)
            case "${2:-}" in
                true|realistic|hostile) TREE="SAMPLE=1 SAMPLE_MIX=$2"; shift 2 ;;
                *) TREE="SAMPLE=1"; shift ;;
            esac ;;
        --demo) TREE="DEMO=1"; shift ;;
        --no-build) BUILD=0; shift ;;
        -h|--help) usage 0 ;;
        *) echo "unknown option $1" >&2; usage 1 ;;
    esac
done

[ -x "$ARES" ] || { echo "ares not found at $ARES (set ARES=)" >&2; exit 1; }

# Warn rather than refuse: an interactive look at the UI is still useful with the harness off,
# unlike a measurement run. But say so, because "no dumps appeared" is otherwise mystifying.
# Defocus must not be Pause. ares with "Defocus: Pause" stops emulating the moment its window
# loses focus, which a background or headless run never has -- so the ROM never reaches its exit
# opcode and the run dies on the timeout with an EMPTY log, because ares buffers stdout and a
# killed process never flushes it. That looks exactly like a ROM that hangs. It cost an hour of
# bisecting working code before the setting was found, so it is asserted here now.
grep -qE "^[[:space:]]*Defocus:[[:space:]]*Allow" "$SETTINGS" 2>/dev/null || {
    echo "ares Defocus is not Allow in $SETTINGS -- a background run will pause and time out" >&2
    exit 1
}

for setting in HomebrewMode ExpansionPak; do
    grep -qE "^[[:space:]]*$setting:[[:space:]]*true" "$SETTINGS" 2>/dev/null \
        || echo "warning: ares $setting is not true in $SETTINGS" >&2
done

if [ "$BUILD" = 1 ]; then
    if [ -n "$SCRIPT" ]; then
        [ -f "$SCRIPT" ] || { echo "no such script: $SCRIPT" >&2; exit 1; }
        # shellcheck disable=SC2086  # TREE is a deliberate word list of make variables
        make $TREE DEV_HARNESS=1 INPUT_SCRIPT="$SCRIPT" sc64 -j8
    else
        # shellcheck disable=SC2086
        make $TREE sc64 -j8
    fi
fi

exec "$ARES" --system "Nintendo 64" --no-file-prompt output/sc64menu.n64
