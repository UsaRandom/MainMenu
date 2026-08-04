#!/bin/sh
# Build, run headlessly under ares, and hash every frame the ROM dumped.
#
# Two builds are compared by diffing the hash files:
#
#     tools/regress.sh -o build/regress/before
#     ...make a change...
#     tools/regress.sh -o build/regress/after
#     diff build/regress/before/hashes.txt build/regress/after/hashes.txt
#
# Frames are dumped from inside the emulator rather than screenshotted, so what lands here is
# what the RDP produced and not what a compositor or a scaler made of it.
#
# Runs end on their own: the input script finishes with `exit`, which asks ares to quit. The
# timeout only catches a ROM that never gets there, so a normal run costs what it costs rather
# than always costing the timeout.
#
# Usage: tools/regress.sh [-o DIR] [-t SECONDS] [script...]
#        with no scripts, every file in tools/inputs is run.

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=build/regress/run
TIMEOUT=120
ARES="${ARES:-/Volumes/Storage/tools/ares-v148/ares-v148/ares.app/Contents/MacOS/ares}"
SETTINGS="${ARES_SETTINGS:-$HOME/Library/Application Support/ares/settings.bml}"

usage() { sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT=$2; shift 2 ;;
        -t) TIMEOUT=$2; shift 2 ;;
        -h|--help) usage 0 ;;
        --) shift; break ;;
        -*) echo "unknown option $1" >&2; usage 1 ;;
        *) break ;;
    esac
done

[ -x "$ARES" ] || { echo "ares not found at $ARES (set ARES=)" >&2; exit 1; }

# Refuse rather than produce an empty log. With Homebrew Mode off the EMUX opcodes are ignored,
# so there are no dumps and no self-termination -- a run that looks exactly like a ROM that
# crashed before reaching the instrumentation. With the Expansion Pak off the menu is running
# on 4 MB, which is not the M64 and not what any measurement here is about.
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
    if ! grep -qE "^[[:space:]]*$setting:[[:space:]]*true" "$SETTINGS" 2>/dev/null; then
        echo "ares $setting is not true in $SETTINGS -- fix it before measuring anything" >&2
        exit 1
    fi
done

if [ $# -gt 0 ]; then
    SCRIPTS=$*
else
    SCRIPTS=$(ls tools/inputs/*.txt)
fi

mkdir -p "$OUT"
: > "$OUT/hashes.txt"

for script in $SCRIPTS; do
    name=$(basename "$script" .txt)
    echo "== $name"

    # idle.txt is the one script that needs the library to have SETTLED, and the real art corpus
    # cannot settle inside its 1,200 frames -- one card in it decodes for 38 seconds on its own.
    # Built against the real fixture the run is still mid-image at the dump, so the gate reported
    # "no allocations" from a frame that was nowhere near the state it is asking about. See the
    # PLAIN_ART comment in the Makefile and AUDIT.md 1u.
    case "$name" in idle) EXTRA=PLAIN_ART=1 ;; *) EXTRA= ;; esac

    make FIXTURE=1 DEV_HARNESS=1 INPUT_SCRIPT="$script" $EXTRA sc64 -j8 >"$OUT/$name.build.log" 2>&1 || {
        echo "build failed; see $OUT/$name.build.log" >&2
        exit 1
    }

    log="$OUT/$name.ares.log"
    "$ARES" --system "Nintendo 64" --no-file-prompt output/sc64menu.n64 >"$log" 2>&1 &
    pid=$!

    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$TIMEOUT" ]; do
        sleep 1
        waited=$((waited + 1))
    done

    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        echo "  TIMEOUT after ${TIMEOUT}s -- the ROM never asked to exit" >&2
        echo "$name TIMEOUT" >> "$OUT/hashes.txt"
        continue
    fi
    wait "$pid" 2>/dev/null || true

    # A run that produced no dumps is a failure, not an empty success. Without this the
    # hash file stays empty and the diff against a previous run reports no change.
    if ! grep -q "^FBDUMP" "$log"; then
        echo "  NO FRAMES dumped -- see $log" >&2
        echo "$name NOFRAMES" >> "$OUT/hashes.txt"
        continue
    fi

    python3 tools/fbdump2png.py "$log" --hashes | sed "s/^/$name /" >> "$OUT/hashes.txt"
    python3 tools/fbdump2png.py "$log" -o "$OUT/$name" >/dev/null
    echo "  $(grep -c "^$name " "$OUT/hashes.txt") frames -> $OUT/$name"
done

echo
echo "hashes: $OUT/hashes.txt"
cat "$OUT/hashes.txt"
