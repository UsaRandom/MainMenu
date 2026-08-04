#!/usr/bin/env python3
"""
Compile a plain-text input script into the C table src/dev/inputscript.c replays.

ares cannot replay input, so the script rides along inside the ROM. Steps are keyed on frame
number rather than elapsed time: ares runs at whatever speed the host allows -- a single
shader compile stall costs tens of milliseconds -- so a time-based script would land on
different UI states from run to run and the framebuffer hashes would never settle.

Script syntax, one directive per line, # comments to end of line:

    wait N              hold nothing for N frames
    press BUTTON [xN]   press once (or N times), each for `hold` frames then `gap` idle frames
    hold DIR N          hold a direction for N frames
    cpress DIR [xN]     same as press but on the C-pad, which sets go_fast
    fbdump              dump the frame rendered at this point
    exit                ask ares to quit; ends a headless run without waiting for a timeout
    set hold N          frames a press is held        (default 2)
    set gap N           idle frames after a press     (default 6)

BUTTON is a b start l r z, or a direction: up down left right.
DIR is up down left right upleft upright downleft downright.

  tools/mkinput.py tools/inputs/scroll-4rows.txt -o build/inputscript_generated.h

Called with no input file it still emits the header with an empty program, so a DEV_HARNESS
build without a script compiles and runs interactively.
"""

import argparse
import os
import sys

# Mirrors joypad_8way_t in libdragon's joypad.h. Verified against the installed header at
# generation time -- see check_joypad_enum(), because a silent renumbering here would steer
# the menu in the wrong direction and read as a UI bug.
DIRS = {
    "right": 0, "upright": 1, "up": 2, "upleft": 3,
    "left": 4, "downleft": 5, "down": 6, "downright": 7,
}
NONE = 0xFF

# Must match ISCRIPT_BTN_* in src/dev/inputscript.h. "cright" is the Fav button: it is a button
# and not a C-pad direction, so it is spelled `press cright`, never `cpress right`.
BUTTONS = {"a": 1 << 0, "b": 1 << 1, "start": 1 << 2, "l": 1 << 3, "r": 1 << 4, "z": 1 << 5,
           "cright": 1 << 6}

ACT_NONE, ACT_FBDUMP, ACT_EXIT = 0, 1, 2


def check_joypad_enum():
    """Confirm our direction numbering still matches libdragon's joypad_8way_t."""
    inst = os.environ.get("N64_INST", os.path.expanduser("~/n64inst-preview"))
    header = os.path.join(inst, "mips64-elf", "include", "joypad.h")
    try:
        with open(header, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return  # not fatal; the build will still work if the enum is unchanged

    order = [n for n in ("RIGHT", "UP_RIGHT", "UP", "UP_LEFT",
                         "LEFT", "DOWN_LEFT", "DOWN", "DOWN_RIGHT")]
    want = ["JOYPAD_8WAY_" + n for n in order]
    seen = [line.strip().split("=")[0].strip().rstrip(",")
            for line in text.splitlines() if "JOYPAD_8WAY_" in line and "=" in line]
    seen = [s for s in seen if s in want]
    if seen and seen != want:
        sys.exit("joypad_8way_t order changed in %s: got %s" % (header, seen))


class Script:
    def __init__(self):
        self.events = []
        self.frame = 0
        self.hold = 2
        self.gap = 6

    def add(self, dir_=NONE, cdir=NONE, buttons=0, hold=None, action=ACT_NONE):
        hold = self.hold if hold is None else hold
        self.events.append((self.frame, hold, dir_, cdir, buttons, action))
        self.frame += hold

    def idle(self, n):
        self.frame += n


def parse(lines, path):
    s = Script()
    for lineno, raw in enumerate(lines, 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        cmd = parts[0].lower()

        def die(msg):
            sys.exit("%s:%d: %s" % (path, lineno, msg))

        if cmd == "set":
            if len(parts) != 3 or parts[1] not in ("hold", "gap"):
                die("expected 'set hold N' or 'set gap N'")
            setattr(s, parts[1], int(parts[2]))

        elif cmd == "wait":
            if len(parts) != 2:
                die("expected 'wait N'")
            s.idle(int(parts[1]))

        elif cmd in ("press", "cpress"):
            if len(parts) not in (2, 3):
                die("expected '%s BUTTON [xN]'" % cmd)
            what = parts[1].lower()
            count = int(parts[2][1:]) if len(parts) == 3 and parts[2].lower().startswith("x") else 1
            for _ in range(count):
                if what in DIRS:
                    if cmd == "cpress":
                        s.add(cdir=DIRS[what])
                    else:
                        s.add(dir_=DIRS[what])
                elif what in BUTTONS:
                    if cmd == "cpress":
                        die("cpress takes a direction, not %r" % what)
                    s.add(buttons=BUTTONS[what])
                else:
                    die("unknown button or direction %r" % what)
                s.idle(s.gap)

        elif cmd == "hold":
            if len(parts) != 3 or parts[1].lower() not in DIRS:
                die("expected 'hold DIR N'")
            s.add(dir_=DIRS[parts[1].lower()], hold=int(parts[2]))

        elif cmd == "fbdump":
            s.add(hold=1, action=ACT_FBDUMP)

        elif cmd == "exit":
            s.add(hold=1, action=ACT_EXIT)

        else:
            die("unknown directive %r" % cmd)

    return s


HEADER = """/* Generated by tools/mkinput.py -- do not edit.
 * source: %s
 * %d events, %d frames
 */
#ifndef INPUTSCRIPT_GENERATED_H__
#define INPUTSCRIPT_GENERATED_H__

#define INPUTSCRIPT_NAME "%s"
#define INPUTSCRIPT_EVENT_COUNT %d

/* Always at least one element: a zero-length array is a constraint violation, and sizeof on
 * one is not usefully zero. INPUTSCRIPT_EVENT_COUNT is the authority on how many are real, so
 * an empty program leaves inputscript_active() false and real input passes through. */
static const input_event_t INPUTSCRIPT_EVENTS[] = {
%s};

#endif /* INPUTSCRIPT_GENERATED_H__ */
"""


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", nargs="?", help="input script; omit for an empty program")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    check_joypad_enum()

    if args.script:
        with open(args.script, "r", encoding="utf-8") as f:
            s = parse(f.readlines(), args.script)
        name = os.path.splitext(os.path.basename(args.script))[0]
        source = args.script
    else:
        s = Script()
        name, source = "", "(none)"

    body = "".join("    { %5d, %5d, 0x%02X, 0x%02X, 0x%02X, %d },\n" % e for e in s.events)
    if not s.events:
        body = "    { 0, 0, 0xFF, 0xFF, 0x00, 0 },  /* placeholder; count is 0 */\n"

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(HEADER % (source, len(s.events), s.frame, name, len(s.events), body))

    print("inputscript: %s -> %s (%d events, %d frames)"
          % (source, args.output, len(s.events), s.frame))
    return 0


if __name__ == "__main__":
    sys.exit(main())
