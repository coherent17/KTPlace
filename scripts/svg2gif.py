#!/usr/bin/env python3
"""Aggregate the per-iteration SVG frames of a placement run into an animated GIF.

Usage:
    scripts/svg2gif.py <plots_dir> [-o out.gif] [--fps N] [--scale S] [--max-frames N]

The plots directory is the one KTPlace writes per iteration (the <name>_plots
folder): it contains frame_step_000.svg, ... plus hpwl.csv and index.html.  All
frame_step_*.svg files are converted to PNG with cairosvg and stitched into a
looping GIF with Pillow.

Dependencies (not vendored):
    pip install cairosvg pillow
"""

import argparse
import glob
import os
import re
import sys


def iter_frames(plots_dir):
    pattern = os.path.join(plots_dir, "frame_step_*.svg")
    frames = glob.glob(pattern)

    def step_num(path):
        match = re.search(r"frame_step_(\d+)\.svg$", path)
        return int(match.group(1)) if match else sys.maxsize

    return sorted(frames, key=step_num)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plots_dir", help="directory holding frame_step_*.svg")
    parser.add_argument("-o", "--out", help="output gif path (default: <plots_dir>.gif)")
    parser.add_argument("--fps", type=int, default=5, help="animation speed (default 5)")
    parser.add_argument("--scale", type=float, default=1.0, help="downscale factor (e.g. 0.5)")
    parser.add_argument("--max-frames", type=int, default=0,
                        help="stop after this many frames (0 = all)")
    args = parser.parse_args(argv)

    if not os.path.isdir(args.plots_dir):
        parser.error("not a directory: " + args.plots_dir)

    import cairosvg
    from PIL import Image

    frames = iter_frames(args.plots_dir)
    if not frames:
        sys.exit("no frame_step_*.svg found in " + args.plots_dir)
    if args.max_frames and args.max_frames < len(frames):
        frames = frames[: args.max_frames]

    print(f"rendering {len(frames)} frames with cairosvg ...", file=sys.stderr)
    images = []
    for i, path in enumerate(frames, 1):
        png = cairosvg.svg2png(url=path, scale=args.scale)
        img = Image.open(__import__("io").BytesIO(png)).convert("RGBA")
        images.append(img)
        print(f"  [{i}/{len(frames)}] {os.path.basename(path)}", file=sys.stderr)

    out = args.out or (args.plots_dir.rstrip(os.sep) + ".gif")
    duration = 1000.0 / args.fps  # ms per frame
    images[0].save(out, save_all=True, append_images=images[1:], duration=duration,
                   loop=0, dispose=2)
    print(f"wrote {out} ({len(frames)} frames, {args.fps} fps)", file=sys.stderr)


if __name__ == "__main__":
    main()