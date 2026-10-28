#!/usr/bin/env python3
"""Make a mixed-cell-height variant of a Bookshelf design.

Following the experiment of Li et al., "Routability-Driven and Fence-Aware
Legalization for Mixed-Cell-Height Circuits" (DAC 2018, Table 2): a fraction
of the movable cells (every tenth, by default) becomes double height and half
width. Pin offsets of those cells are scaled with them.

    python3 scripts/make_mixed_height.py benchmark/bookshelf/ISPD_2005/adaptec1 \
        output/adaptec1_md
"""

import argparse
import gzip
import pathlib
import shutil


def open_text(path, mode):
    if path.suffix == ".gz":
        return gzip.open(path, mode + "t")
    return open(path, mode)


def find(directory, suffix):
    for path in sorted(directory.iterdir()):
        if path.name.endswith(suffix) or path.name.endswith(suffix + ".gz"):
            return path
    raise SystemExit(f"no {suffix} file in {directory}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("design", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--every", type=int, default=10, help="convert every Nth movable cell")
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    for path in args.design.iterdir():
        if path.is_file():
            shutil.copy(path, args.output / path.name)

    scale = {}
    nodes = find(args.design, ".nodes")
    lines = []
    movable = 0
    with open_text(nodes, "r") as source:
        for line in source:
            fields = line.split()
            if len(fields) == 3 and fields[0] not in ("NumNodes", "NumTerminals"):
                try:
                    width = float(fields[1])
                    height = float(fields[2])
                except ValueError:
                    lines.append(line)
                    continue
                if movable % args.every == 0 and width >= 2.0:
                    narrow = float(int(width // 2))
                    scale[fields[0]] = (narrow / width, 2.0)
                    line = f"\t{fields[0]}\t{narrow:g}\t{height * 2:g}\n"
                movable += 1
            lines.append(line)
    with open_text(args.output / nodes.name, "w") as target:
        target.writelines(lines)

    nets = find(args.design, ".nets")
    lines = []
    with open_text(nets, "r") as source:
        for line in source:
            fields = line.split()
            if len(fields) >= 5 and fields[0] in scale and fields[2] == ":":
                sx, sy = scale[fields[0]]
                line = f"\t{fields[0]}\t{fields[1]} : {float(fields[3]) * sx:g}\t{float(fields[4]) * sy:g}\n"
            lines.append(line)
    with open_text(args.output / nets.name, "w") as target:
        target.writelines(lines)
    print(f"{len(scale)} of {movable} movable cells made double height in {args.output}")


if __name__ == "__main__":
    main()
