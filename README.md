# KTPlace
An open-source know thyself placement engine.

## Dependencies

g++ 13+ (C++23), oneTBB, Boost (Iostreams, Test), zlib, fmt, clang-format.

```sh
sudo apt-get install -y --no-install-recommends \
    g++ libtbb-dev libboost-iostreams-dev libboost-test-dev zlib1g-dev libfmt-dev clang-format
```

System packages, not submodules: every distribution ships these, and vendoring
them would only pin versions behind the distribution's own updates. CImg, which
is *not* packaged usefully, is vendored as a single header under CeCILL-C and
CeCILL; see the header in `src/visualization/` for its terms.

## Build

```sh
make            # builds build/bin/ktplace
make test       # build and run every unit suite
make format     # clang-format the sources
```

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
image library beyond the vendored CImg, no external tools.

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
| `simpl` | `trace_every` | SVG frame every N global iterations; `0` turns them off (`1`) |
| `simpl` | `cg_every` | SVG frame every N conjugate-gradient iterates; `0` turns them off (`0`) |
| `simpl` | `init_net` | initial placement net model: `star` (placement-independent star model), `b2b` (the paper's Bound2Bound), or `star_b2b` (`star`) |
| `simpl` | `grid`, `cg_tol`, `init_iters`, `iters`, `seed`, `fences`, `density`, `alpha_base` | solver settings |
| `ntuplace1` | `leaf_cells`, `max_levels`, `retries`, `verbose` | partitioner settings |
| `abacus` | `max_row_dist`, `min_subrow_sites` | legalizer settings |
| `abacus` | `frame_every` | SVG frame every N cells placed; `0` keeps only the first and last (`0`) |
| `multirow` | `macro_rows` | cells spanning more rows than this are legalized first as macros (`8`) |
| `fastdp` | `window`, `no_median` | detailed placer settings |

Each component's keys and defaults are listed in its `*_options.h`. An unknown
component is an error; a value of the wrong type is reported and the default is
used.

On a large design the frames are most of global placement's time. When only the
placement matters, set `simpl trace_every 0` (the HPWL is the same), or pass
`--no-plots` to skip every picture.

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
