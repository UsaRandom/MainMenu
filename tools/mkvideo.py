#!/usr/bin/env python3
"""
Turn a `record on` ares log into an MP4 (and optionally a GIF).

Frames come out of the emulator, not off a screen capture, so what lands in the video is what
the RDP produced -- no compositor, no window scaler, and no dependence on how fast the host was
running ares at the time. The input script counts frames, so a take written as 650 frames plays
back as 650 frames at 60 fps whatever the emulator's wall-clock speed was.

  tools/regress.sh -t 900 -m 'DEMO=1 FBSCALE=1' -o build/demo-video \\
      tools/inputs/manual/demo-video.txt
  tools/mkvideo.py build/demo-video/demo-video.ares.log -o docs/images/demo.mp4 --gif

This does not reuse tools/fbdump2png.py, and the reason is scale rather than taste. That script
holds every frame in memory and converts pixels in a Python loop, which is right for the nine
frames a regression script dumps and impossible for the 650 in a video: 200 million iterations
of the inner loop, and 400 MB of framebuffers resident. Here each frame is converted with numpy
and written straight down a pipe to ffmpeg, so memory is one frame and the cost is linear.
"""

import argparse
import itertools
import os
import subprocess
import sys

try:
    import numpy as np
except ImportError:
    sys.exit("mkvideo.py needs numpy (pip install numpy)")


def frames(path):
    """Yield (width, height, rgb24 bytes) for each dump in the log, one at a time."""
    width = height = 0
    buf = bytearray()
    inside = False
    n_short = 0

    with open(path, "rb") as f:
        for line in f:
            if b"FBDUMP" in line:
                # FBDUMP w=640 h=480 scale=1 fmt=rgba5551
                fields = dict(tok.split(b"=", 1) for tok in line.split() if b"=" in tok)
                if b"w" not in fields or b"fmt" not in fields:
                    continue
                if fields[b"fmt"] != b"rgba5551":
                    sys.exit("unsupported pixel format %r" % fields[b"fmt"])
                width, height = int(fields[b"w"]), int(fields[b"h"])
                buf = bytearray()
                inside = True
                continue

            if b"FBEND" in line:
                if inside:
                    want = width * height * 2
                    if len(buf) < want:
                        # Padded rather than dropped: one short frame in the middle of a take
                        # would silently shorten the video and shift everything after it.
                        n_short += 1
                        buf.extend(b"\x00" * (want - len(buf)))
                    yield width, height, to_rgb(buf[:want], width, height)
                inside = False
                continue

            if not inside:
                continue

            # ares hex dump: "ffffffffa00f2300 0000: 00 11 22 ...  |........|"
            # Split on the offset colon and cut the ASCII gutter. A regex per line costs about
            # a third of the total runtime over 30 million lines, and this shape is fixed.
            try:
                body = line.split(b": ", 1)[1]
            except IndexError:
                continue
            bar = body.find(b"|")
            if bar >= 0:
                body = body[:bar]
            try:
                buf.extend(bytes.fromhex(body.decode("ascii", "ignore")))
            except ValueError:
                # Not a hex line. debugf output interleaves with the dump -- the menu keeps
                # logging while ares is emitting the frame -- and a stray line must be stepped
                # over rather than shifting every byte after it by however long it was.
                continue

    if n_short:
        print("warning: %d frame(s) were short and got padded" % n_short, file=sys.stderr)


def to_rgb(data, width, height):
    """RGBA5551 big-endian -> packed RGB24."""
    px = np.frombuffer(bytes(data), dtype=">u2").reshape(height, width)
    out = np.empty((height, width, 3), dtype=np.uint8)
    for i, sh in enumerate((11, 6, 1)):
        c = ((px >> sh) & 0x1F).astype(np.uint8)
        # 5 bits to 8 by replicating the high bits, so full scale stays full scale. Multiplying
        # by 8 instead leaves white at 248 and the whole image slightly grey.
        out[:, :, i] = (c << 3) | (c >> 2)
    return out.tobytes()


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("-o", "--output", default="build/demo.mp4")
    ap.add_argument("--fps", type=int, default=60,
                    help="playback rate; the N64 renders 60 fields a second (default 60)")
    ap.add_argument("--scale", type=int, default=1,
                    help="integer upscale, nearest-neighbour, for a dump taken below 640x480")
    ap.add_argument("--gif", action="store_true",
                    help="also write a GIF beside the MP4; GitHub plays a GIF inline in a "
                         "README and does not play an MP4 from the repo")
    ap.add_argument("--gif-fps", type=int, default=20)
    ap.add_argument("--gif-width", type=int, default=480)
    args = ap.parse_args()

    src = frames(args.log)
    try:
        first = next(src)
    except StopIteration:
        sys.exit("no framebuffer dumps in %s -- did the script say `record on`?" % args.log)

    w, h, _ = first
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)

    vf = []
    if args.scale > 1:
        vf.append("scale=%d:%d:flags=neighbor" % (w * args.scale, h * args.scale))

    cmd = ["ffmpeg", "-y", "-loglevel", "error",
           "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (w, h),
           "-framerate", str(args.fps), "-i", "-",
           # yuv420p and even dimensions, or QuickTime and most browsers refuse to play it.
           "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-pix_fmt", "yuv420p",
           "-movflags", "+faststart"]
    if vf:
        cmd += ["-vf", ",".join(vf)]
    cmd.append(args.output)

    proc = subprocess.Popen(cmd, stdin=subprocess.PIPE)
    n = 0
    try:
        for _w, _h, rgb in itertools.chain([first], src):
            proc.stdin.write(rgb)
            n += 1
            if n % 60 == 0:
                print("  %d frames" % n, file=sys.stderr)
    finally:
        proc.stdin.close()
        proc.wait()

    if proc.returncode != 0:
        sys.exit("ffmpeg failed with %d" % proc.returncode)

    print("%s: %d frames, %dx%d at %d fps, %.1f s, %.1f MB"
          % (args.output, n, w * args.scale, h * args.scale, args.fps, n / float(args.fps),
             os.path.getsize(args.output) / 1e6))

    if args.gif:
        gif = os.path.splitext(args.output)[0] + ".gif"
        # Two passes. A GIF made without its own palette dithers against the default 216-colour
        # web palette and turns the box art into confetti.
        pal = os.path.join(os.path.dirname(os.path.abspath(gif)), ".palette.png")
        filt = "fps=%d,scale=%d:-1:flags=lanczos" % (args.gif_fps, args.gif_width)
        subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error", "-i", args.output,
                               "-vf", filt + ",palettegen=stats_mode=diff", pal])
        subprocess.check_call(["ffmpeg", "-y", "-loglevel", "error", "-i", args.output,
                               "-i", pal, "-lavfi",
                               filt + "[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3", gif])
        os.remove(pal)
        print("%s: %.1f MB" % (gif, os.path.getsize(gif) / 1e6))

    return 0



if __name__ == "__main__":
    sys.exit(main())
