# KTPlace
An open-source know thyself placement engine.

![SimPL global placement, legalization and detailed placement of adaptec1](docs/images/adaptec1.gif)

adaptec1 (ISPD 2005) with the default flow: SimPL global placement, Abacus
legalization and FastDP detailed placement, ending at an HPWL of 7.94e7.

## Install a release

Each [release](https://github.com/coherent17/KTPlace/releases) ships two Linux
x86-64 builds of the same engine:

| archive | runs on | needs |
| --- | --- | --- |
| `ktplace-vX.Y.Z-linux-x86_64-static.tar.gz` | any x86-64 Linux | nothing: everything, the C/C++ runtimes included, is linked in |
| `ktplace-vX.Y.Z-linux-x86_64.tar.gz` | Ubuntu 24.04 or newer | nothing beyond the base system (glibc 2.38+, libstdc++ from GCC 13.2+) |

```sh
sha256sum -c ktplace-v1.0.0-linux-x86_64-static.tar.gz.sha256
tar -xzf ktplace-v1.0.0-linux-x86_64-static.tar.gz
source ktplace-v1.0.0-linux-x86_64-static/ktplace.sh
ktplace --version
```

## Dependencies

KTPlace is self-contained: its libraries (oneTBB, zlib, CImg) are git submodules
under `third_party/` and are built into the binary, so clone with them:

```sh
git clone --recurse-submodules https://github.com/coherent17/KTPlace.git
```

All you need installed is:

| tool | version |
| --- | --- |
| C++ compiler | g++ 13+ (Linux) or Apple clang 15+ (macOS), C++23 |
| make | GNU make |

## Build

```sh
make            # builds build/bin/ktplace
make test       # build and run every unit suite
make format     # clang-format the sources
make STATIC=1   # build-static/bin/ktplace, with glibc and libstdc++ linked in too
source ktplace.sh   # written by the build: puts ktplace on PATH
```

## Usage

```sh
ktplace <input_dir> [options]
ktplace ./benchmark/bookshelf/ICCAD04/ibm01 -w ./output/ibm01
```

A directory with a `.def`/`.def.gz` is read as LEF/DEF, anything else as
Bookshelf; gzipped inputs are read directly.

| option | meaning |
| --- | --- |
| `-w, --work-dir <dir>` | where everything is written (default: cwd) |
| `-tcl, --tcl <file>` | Tcl script that configures the run |
| `-v, --verbose` | also echo the trace log to the console |

Everything a run writes goes under `-w`: `placed.pl`, `plots/` (`placement.gif`,
the final `placement.png` and SimPL's `simpl_convergence.png`), `ktplace.log` and `ktplace_trace.log`. stdout is never
written.

## Configuration

A Tcl script passed with `-tcl` sets options with
`add_option <component> <key> <value>`; anything not set keeps its default:

```tcl
add_option flow algorithm ntuplace1   # simpl is the default
add_option simpl grid 64
add_option visualizer anim 0
```

| component | key | effect (default) |
| --- | --- | --- |
| `flow` | `algorithm` | global placer: `simpl` or `ntuplace1` (`simpl`) |
| `visualizer` | `plot` | draw anything at all: frames, GIF, final still; `0` draws nothing (`1`) |
| `visualizer` | `anim` | record frames and assemble the GIF (`1`) |
| `visualizer` | `anim_max_frames` | frame budget for the run (`1200`) |
| `visualizer` | `anim_zoom` | frame scale, vs 768x768 (`2`) |
| `visualizer` | `anim_blend` | GIF frames per placement step: the snapshot plus N-1 frames interpolated from the previous one; `1` turns blending off (`3`) |
| `visualizer` | `final_zoom` | final still scale, vs 768x768 (`8`) |
| `visualizer` | `keep_snapshots` | keep `plots/snapshots.ktsnap` after the pictures are drawn (`0`) |
| `visualizer` | `svg_frames` | also write every frame as an SVG under `plots/` (`0`) |
| `simpl` | `trace_every` | frame every N global iterations; `0` turns them off (`2`) |
| `simpl` | `trace_series` | which placement those frames show: `lss` (the solver's, spreading out smoothly), `lal` (the look-ahead legalized one), or `both` (`lss`) |
| `simpl` | `report_every` | a row of the convergence table (lower and upper bound, gap, overflow) every N iterations; `0` turns it off (`5`) |
| `simpl` | `cg_every` | frame every N conjugate-gradient iterates of global placement; `0` turns them off (`0`) |
| `simpl` | `init_cg_every` | frame every N conjugate-gradient iterates of the first initial-placement solve; `0` turns them off (`4`) |
| `simpl` | `init_net` | initial placement net model: `star` (placement-independent star model), `b2b` (the paper's Bound2Bound), or `star_b2b` (`star`) |
| `simpl` | `grid`, `cg_tol`, `init_iters`, `iters`, `seed`, `fences`, `density`, `alpha_base` | solver settings |
| `ntuplace1` | `leaf_cells`, `max_levels`, `retries`, `verbose` | partitioner settings |
| `abacus` | `max_row_dist`, `min_subrow_sites` | legalizer settings |
| `abacus`, `multirow` | `frames` | frames while legalizing, showing the cells placed so far at their legal positions; `0` keeps only the first and last (`24`) |
| `multirow` | `macro_rows` | cells spanning more rows than this are legalized first as blocks, before the standard cells are legalized around them (`3`) |
| `fastdp` | `window`, `wt1`, `wt2`, `max_iterations`, `min_improvement` | detailed placer: local re-ordering window (`3`), overlap penalty weights (`0.1`, `1`), iteration limit and stop threshold |

Each component's keys and defaults are in its `*_options.h`. On a large design,
`add_option visualizer plot 0` saves all of the drawing time.

## The algorithm

- Global placement: SimPL (Kim, Lee, Markov, CACM 2013; IEEE TCAD 2012), with
  NTUPlace as an alternative.
- Legalization: Abacus (Spindler, Schlichtmann, Johannes, ISPD 2008), and a
  multi-row legalizer for cells taller than a row.
- Detailed placement: FastDP (Pan, Viswanathan, Chu, ICCAD 2005).
- Every stage is followed by an independent placement check (overlaps, die,
  fences).

## Benchmarks

ISPD 2005, ICCAD04 and ISPD 2015 are committed, gzipped, under
`benchmark/bookshelf/` and `benchmark/lefdef/`. CI places `adaptec1` (smoke) and
`ibm01` (the multi-row design). [`benchmark/bm_status.md`](benchmark/bm_status.md)
compares our results with SimPL and DREAMPlace 3.0.

```sh
python3 benchmark/fetch_benchmarks.py            # fetch what is missing, e.g. ISPD 2006
```

See [benchmark/README.md](benchmark/README.md).

## Contributing

Tests and formatting need Boost.Test and clang-format
(`libboost-test-dev clang-format` on Ubuntu, `boost clang-format` on Homebrew).
Work on a branch and open a PR; CI must pass. Enable the pre-commit hook once
(format check and unit tests; skip with `KTPLACE_SKIP_HOOKS=1`):

```sh
make hooks
```

Placements are deterministic, so a change that should not move results must
leave `placed.pl` byte-identical on adaptec1 and ibm01 against main. A change
that does should say by how much in [`benchmark/bm_status.md`](benchmark/bm_status.md).
Conventions are in [CLAUDE.md](CLAUDE.md).
