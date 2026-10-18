#!/usr/bin/env bash
# Records docs/images/gui_demo.gif: scripts/gui_demo.cc drives the real GUI window,
# grabs a frame after each step, and ffmpeg turns the frames into a GIF.
#
# Usage: scripts/gui_demo.sh [design-dir] [output.gif]
# Needs Qt 6 (as for make GUI=1), ffmpeg, and a display: the window opens on screen.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
design=${1:-benchmark/bookshelf/ISPD_2005/adaptec1}
output=${2:-docs/images/gui_demo.gif}
cd "$root"

command -v ffmpeg >/dev/null || { echo "gui_demo: ffmpeg is required" >&2; exit 1; }
make -C src -f Master.make -j"$(nproc)" ../build/bin/gui_demo

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/frames" "$scratch/home" "$scratch/work"

HOME="$scratch/home" build/bin/gui_demo "$scratch/work" "$design" "$scratch/frames"

frames="$scratch/frames"
last=$(ls "$frames"/f*.png | tail -1)
{ cat "$frames/frames.txt"; echo "file $(basename "$last")"; } > "$frames/list.txt"
ffmpeg -loglevel error -y -f concat -safe 0 -i "$frames/list.txt" \
    -vf "scale=1000:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=96:stats_mode=diff[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
    -fps_mode vfr -loop 0 "$output"
echo "gui_demo: wrote $output ($(du -h "$output" | cut -f1))"
