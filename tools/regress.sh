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
# Usage: tools/regress.sh [-o DIR] [-t SECONDS] [-m 'VAR=VAL ...'] [script...]
#        with no scripts, every file in tools/inputs is run.
#
# -m appends make variables to every build. It exists because this script rebuilds the ROM
# itself, so a ROM built by hand beforehand is thrown away -- which silently produced nine
# 160x120 fixture screenshots from a run that had been set up for 640x480 demo ones:
#
#     tools/regress.sh -m 'DEMO=1 FBSCALE=1' -o build/shots tools/inputs/manual/demo-stills.txt

set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

OUT=build/regress/run
TIMEOUT=120
MAKE_EXTRA=
ARES="${ARES:-/Volumes/Storage/tools/ares-v148/ares-v148/ares.app/Contents/MacOS/ares}"
SETTINGS="${ARES_SETTINGS:-$HOME/Library/Application Support/ares/settings.bml}"

# Passed to ares when it is not the default file, because until this existed ARES_SETTINGS only
# chose which file the checks below READ. ares went on loading its own, so pointing this at a
# 4 MB settings file produced a run that validated as 4 MB, ran as 8, and reported that the menu
# boots fine without an Expansion Pak. The heap total in the log was byte-for-byte the 8 MB one,
# which is the only reason it was caught.
ARES_SETTINGS_ARG=
if [ -n "${ARES_SETTINGS:-}" ]; then
    ARES_SETTINGS_ARG="--settings-file $ARES_SETTINGS"
fi

usage() { sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -o) OUT=$2; shift 2 ;;
        -t) TIMEOUT=$2; shift 2 ;;
        -m) MAKE_EXTRA=$2; shift 2 ;;
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

REQUIRED_SETTINGS="HomebrewMode ExpansionPak"

# FOURMB=1 is the one way to run with the Expansion Pak off, and it is deliberately not a way to
# turn the check off -- it INVERTS it. A run that says it is testing 4 MB against a settings file
# with the pak still on would report the 8 MB build passing and prove nothing, which is the same
# shape of mistake the check above exists to prevent. See docs/AUDIT.md on the 4 MB path.
if [ "${FOURMB:-0}" = "1" ]; then
    REQUIRED_SETTINGS=HomebrewMode
    if ! grep -qE "^[[:space:]]*ExpansionPak:[[:space:]]*false" "$SETTINGS" 2>/dev/null; then
        echo "FOURMB=1 but ExpansionPak is not false in $SETTINGS -- that run would be 8 MB" >&2
        exit 1
    fi
    echo "== 4 MB run: Expansion Pak OFF"
fi

for setting in $REQUIRED_SETTINGS; do
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

    make FIXTURE=1 DEV_HARNESS=1 INPUT_SCRIPT="$script" $EXTRA $MAKE_EXTRA sc64 -j8 >"$OUT/$name.build.log" 2>&1 || {
        echo "build failed; see $OUT/$name.build.log" >&2
        exit 1
    }

    log="$OUT/$name.ares.log"
    "$ARES" --system "Nintendo 64" $ARES_SETTINGS_ARG --no-file-prompt output/sc64menu.n64 >"$log" 2>&1 &
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
