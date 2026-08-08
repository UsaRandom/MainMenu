#!/bin/sh
# Boot a ROM in ares, screenshot the screen after N seconds, and quit.
#
#     tools/aresshot.sh ROM.z64 out.png [seconds]
#
# The cheat work has no debugger and no serial line on either target, so "did the patched image
# boot, and did the handler run" is answered by looking at a picture. tools/run.sh is for the
# menu and drives the build; this is the bare round trip for an arbitrary ROM, which is what
# tools/rompatch.py needs to be worth using.
#
# Whole-screen capture rather than the window, deliberately: `screencapture -l` needs a window id
# that ares does not advertise, and a full-screen shot also catches ares' own error dialogs --
# which is exactly the case where a window-only capture would come back empty and mystifying.

set -eu

ROM=${1:?usage: aresshot.sh ROM.z64 OUT.png [seconds]}
OUT=${2:?usage: aresshot.sh ROM.z64 OUT.png [seconds]}
WAIT=${3:-20}

ARES="${ARES:-/Volumes/Storage/tools/ares-v148/ares-v148/ares.app/Contents/MacOS/ares}"
[ -x "$ARES" ] || { echo "ares not found at $ARES (set ARES=)" >&2; exit 1; }

"$ARES" --system "Nintendo 64" --no-file-prompt "$ROM" >"${OUT%.png}.log" 2>&1 &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT INT TERM

sleep "$WAIT"
kill -0 $PID 2>/dev/null || { echo "ares exited early -- see ${OUT%.png}.log" >&2; exit 1; }
screencapture -x -o "$OUT"
kill $PID 2>/dev/null || true
wait $PID 2>/dev/null || true
echo "$OUT"
