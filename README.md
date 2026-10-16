# KTPlace
An open-source know thyself placement engine.

![SimPL global placement, legalization and detailed placement of adaptec1](docs/images/adaptec1.gif)

adaptec1 (ISPD 2005) with the default flow: SimPL global placement, Abacus
legalization and FastDP detailed placement, ending at an HPWL of 7.94e7. The
animation was drawn with

```tcl
add_option visualizer anim_zoom 1
add_option visualizer anim_max_frames 70
add_option visualizer anim_blend 1
add_option simpl trace_every 3
```

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

Building and running needs only a C++23 compiler (g++ 13+ on Linux, Apple clang
15+ on macOS) and make. oneTBB (v2021.11.0),
zlib (v1.3.1) and CImg are git submodules under `third_party/`; oneTBB and zlib
are compiled into the binary by the build. Clone with them:

```sh
git clone --recurse-submodules https://github.com/coherent17/KTPlace.git
# or, in an existing clone:
git submodule update --init --recursive
```

Only the unit tests and formatting need packages:

```sh
sudo apt-get install -y --no-install-recommends libboost-test-dev clang-format   # Linux
brew install boost clang-format                                                  # macOS
```

Linux and macOS (Apple Silicon) produce byte-identical placements; CI checks it.
On macOS, build and test with Apple clang (the default): Homebrew's Boost is
built against libc++ and does not link with GCC.

CImg (v.4.0.6, header-only) is the third submodule. CImg is under CeCILL-C and
CeCILL, oneTBB under Apache-2.0 and zlib under the zlib license; see their
directories under `third_party/`.

## Build

```sh
make            # builds build/bin/ktplace
make test       # build and run every unit suite
make format     # clang-format the sources
make STATIC=1   # build-static/bin/ktplace, with glibc and libstdc++ linked in too
```

The first build also compiles oneTBB and zlib from `third_party/` (about 30 s on
16 cores); later builds reuse them.

The build also writes `ktplace.sh` and `ktplace.csh` into the repository root.
Source the one for your shell and the engine is callable by name:

```sh
source ktplace.sh
```

### Docker (any OS)

On macOS, Windows, or a Linux whose packages differ from CI's, build and run in
a container with exactly CI's toolchain. The repository is mounted live, so you
edit on the host and build in Linux:

```sh
scripts/devenv.sh up        # start the dev container and compile ktplace
scripts/devenv.sh ssh       # shell in /workspace (or: ssh -p 2222 dev@127.0.0.1)
docker compose up -d        # the same without bash, e.g. from PowerShell
```

`docker build -t ktplace .` builds and unit-tests a copy of the tree the way CI
does. [docs/docker.md](docs/docker.md) covers the rest: running commands, the
web console, settings and troubleshooting.

## Usage

```sh
ktplace <input_dir> [options]
ktplace ./benchmark/ICCAD04/ibm01 -w ./output/ibm01
```

`input_dir` holds the design's files, named after the design. The format is
auto-detected: a `.def`/`.def.gz` goes through the LEF/DEF adapter, everything
else is loaded as Bookshelf.

| option | meaning |
| --- | --- |
| `-w, --work-dir <dir>` | where everything is written (default: cwd) |
| `-tcl, --tcl <file>` | Tcl script that configures the run (see below) |
| `--no-plots` | draw nothing |
| `-v, --verbose` | also echo the trace log to the console |

`-w` is the single artifact root: the run derives `placed.pl`, `plots/` and
`ktplace.log` from it and creates it if missing. The trace always goes to
`ktplace_trace.log`. **stdout is never written to** — the log goes to the file
and stderr, so redirecting stdout stays clean.

| `ktplace.log` | `ktplace_trace.log` | stderr |
|:---:|:---:|:---:|
| echo, fatal | trace | echo, fatal |

## Output

Under `<work-dir>/plots/`: SVG frames for the placement stages (`simpl/`, and
`legalize/` for the Abacus path), the detail placer (`detailplace/`), the density
bound trace (`simpl_bounds.csv`/`.svg`), the animation (`anim/placement.gif`,
unless `visualizer anim` is `0`), and a high-resolution still of the finished
placement (`final/final.png`, 6144x6144 by default). All generated in C++ — no
image library beyond CImg (header-only, in `third_party/`), no external tools.

## Configuration

A run is configured by a Tcl script passed with `-tcl`. Environment variables are
not read. `add_option <component> <key> <value>` sets one option; anything not set
keeps the component's default:

```tcl
add_option flow algorithm ntuplace1   # simpl is the default
add_option simpl grid 64
add_option visualizer anim 0
```

| component | key | effect (default) |
| --- | --- | --- |
| `flow` | `algorithm` | global placer: `simpl` or `ntuplace1` (`simpl`) |
| `visualizer` | `anim` | record frames and assemble the GIF (`1`) |
| `visualizer` | `anim_max_frames` | frame budget for the run (`1200`) |
| `visualizer` | `anim_zoom` | frame scale, vs 768x768 (`2`) |
| `visualizer` | `anim_blend` | GIF frames per placement step: the snapshot plus N-1 frames interpolated from the previous one; `1` turns blending off (`3`) |
| `visualizer` | `final_zoom` | final still scale, vs 768x768 (`8`) |
| `visualizer` | `final_ppm` | also write the lossless PPM beside the PNG (`0`) |
| `visualizer` | `keep_snapshots` | keep `plots/snapshots.ktsnap` after the pictures are drawn (`0`) |
| `simpl` | `trace_every` | SVG frame every N global iterations; `0` turns them off (`1`) |
| `simpl` | `cg_every` | SVG frame every N conjugate-gradient iterates; `0` turns them off (`0`) |
| `simpl` | `init_net` | initial placement net model: `star` (placement-independent star model), `b2b` (the paper's Bound2Bound), or `star_b2b` (`star`) |
| `simpl` | `grid`, `cg_tol`, `init_iters`, `iters`, `seed`, `fences`, `density`, `alpha_base` | solver settings |
| `ntuplace1` | `leaf_cells`, `max_levels`, `retries`, `verbose` | partitioner settings |
| `abacus` | `max_row_dist`, `min_subrow_sites` | legalizer settings |
| `abacus` | `frame_every` | SVG frame every N cells placed; `0` keeps only the first and last (`0`) |
| `multirow` | `macro_rows` | cells spanning more rows than this are legalized first as blocks, before the standard cells are legalized around them (`3`) |
| `fastdp` | `window`, `wt1`, `wt2`, `max_iterations`, `min_improvement` | detailed placer: local re-ordering window (`3`), overlap penalty weights (`0.1`, `1`), iteration limit and stop threshold |

Each component's keys and defaults are listed in its `*_options.h`. An unknown
component is an error; a value of the wrong type is reported and the default is
used.

On a large design drawing the frames takes longer than placing. When only the
placement matters, set `simpl trace_every 0` (the HPWL is the same), or pass
`--no-plots` to skip every picture.

The stages do not draw while they run. They record their snapshots to
`plots/snapshots.ktsnap`, and the flow draws the pictures from it once `placed.pl`
is written, then deletes it (`visualizer keep_snapshots 1` keeps it). On adaptec1 that takes global placement from 26 s
to 10 s. [docs/visualization.md](docs/visualization.md) covers how stages, the
flow and the visualizer fit together, and the snapshot file format.

Timing is measured with `ScopedTimer` (`src/util/kt_scopedTimer.h`), which
records wall and processor time per named phase and reports the table once at
the end. Circuit delay needs a timing graph and cell libraries, so it belongs in
a separate engine.

## The algorithm

SimPL is implemented from the paper, which is the reference for every decision in
`src/placer/simpl/`:

> M.-C. Kim, D.-J. Lee, I. L. Markov. *SimPL: An Algorithm for Placing VLSI
> Circuits.* Communications of the ACM 56(6), June 2013. DOI
> 10.1145/2461256.2461279.

The paper is paywalled and not redistributed here. Where the implementation
departs from it, the departure is stated in a comment at the site of the
decision, with the paper text quoted, so the two can be compared rather than
taken on trust.

Legalization is Abacus for single-row cells and `MultiRowLegalizer` for cells
taller than a row; detailed placement is FastDP. Both are followed by an
independent placement check, and the result is reported per stage.

## Benchmarks

The suites are several GB, so almost nothing is vendored. Two designs are,
because CI needs them:

- `ISPD_2005/adaptec1` — the end-to-end smoke design. That suite has no working
  download URL, so a job that fetched it would be a build failing for reasons
  that have nothing to do with the code.
- `ICCAD04/ibm01` — the only design with cells taller than a row, so the only one
  that reaches the multi-row legalizer.

Fetch the rest from their original publishers:

```sh
python3 benchmark/fetch_benchmarks.py            # everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
```

See [benchmark/README.md](benchmark/README.md).
