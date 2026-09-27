#!/usr/bin/env python3
"""Independent Bookshelf HPWL, for cross-checking placer and legalizer output.

The definition is the one ktplace's SimplePlacer::Impl::hpwl uses, and the one
NTUplace3 calls "Pin-to-pin HPWL":

  * a net contributes (max_x - min_x) + (max_y - min_y) over its pins
  * a pin sits at its instance origin plus the pin offset from the .nets file,
    including pins on fixed terminals
  * nets with fewer than two pins contribute nothing

Computing this ourselves is the only way to compare two tools fairly: each one
reports its own number, and those numbers are only comparable if the same
formula produced them. Run it on any .pl to get a number nobody else can argue
with.

Usage:
  bookshelf_hpwl.py <design.nodes> <design.nets> [design.pl ...]
"""

import gzip
import sys


def _open(path):
    return gzip.open(path, "rt") if path.endswith(".gz") else open(path)


def read_nodes(path):
    """name -> (w, h) for movable cells; terminals too, for reference."""
    size = {}
    with _open(path) as f:
        for line in f:
            if line.startswith(("NumNodes", "NumTerminals", "UCLA", "#", "\n")):
                continue
            parts = line.split()
            if len(parts) >= 3:
                size[parts[0]] = (float(parts[1]), float(parts[2]))
    return size


def read_pl(path):
    """name -> (x, y), plus the set of names the .pl left unplaced."""
    pos = {}
    with _open(path) as f:
        for line in f:
            if line.startswith(("UCLA", "#", "\n")):
                continue
            parts = line.split()
            if len(parts) >= 2:
                pos[parts[0]] = (float(parts[1]), float(parts[2]))
    return pos


def read_nets_into(path, out):
    """Fill `out` with one list of (inst, off_x, off_y) per net.

    "NetDegree : 4   n0" carries a count, not a port list, so the pin lines that
    follow are the members; the name is not used here.
    """
    net = None
    with _open(path) as f:
        for line in f:
            if line.startswith("NetDegree"):
                net = []
                out.append(net)
                continue
            if line.startswith(("NumNets", "NumPins", "UCLA", "#")) or not line.strip():
                continue
            # "o197239\tI : -0.500000\t-6.000000" -- the offsets sit right of the
            # colon, so split there rather than trusting a fixed field index.
            if ":" not in line:
                continue
            head, _, tail = line.partition(":")
            inst = head.split()
            offs = tail.split()
            if not inst or len(offs) < 2:
                continue
            net.append((inst[0], float(offs[0]), float(offs[1])))


def hpwl(nodes_path, nets_path, pl_path, verbose=True):
    pos = read_pl(pl_path)
    nets = []
    read_nets_into(nets_path, nets)

    missing = 0
    total = 0.0
    counted = 0
    for net in nets:
        if len(net) < 2:
            continue
        x0 = y0 = float("inf")
        x1 = y1 = float("-inf")
        for inst, ox, oy in net:
            p = pos.get(inst)
            if p is None:
                missing += 1
                continue
            x = p[0] + ox
            y = p[1] + oy
            if x < x0:
                x0 = x
            if x > x1:
                x1 = x
            if y < y0:
                y0 = y
            if y > y1:
                y1 = y
        if x0 > x1:
            continue
        total += (x1 - x0) + (y1 - y0)
        counted += 1

    if verbose:
        size = read_nodes(nodes_path)
        unplaced = sum(
            1
            for n, (w, h) in size.items()
            if n not in pos or (pos[n][0] == 0.0 and pos[n][1] == 0.0)
        )
        print(f"  nodes            : {len(size)}")
        print(f"  nets             : {len(nets)} ({counted} with >= 2 placed pins)")
        print(f"  pins w/o position: {missing}")
        print(f"  instances at 0,0 : {unplaced}")
    return total


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 1
    nodes, nets = sys.argv[1], sys.argv[2]
    for pl in sys.argv[3:]:
        print(f"{pl}:")
        h = hpwl(nodes, nets, pl)
        print(f"  HPWL             : {h:.6e}")
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
