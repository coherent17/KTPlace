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
does.

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
| `-a, --algorithm <name>` | `simpl` (default) or `ntuplace1` |
| `-w, --work-dir <dir>` | where everything is written (default: cwd) |
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
when `KTPLACE_ANIM` is set), and a high-resolution still of the finished
placement (`final/final.png`, 6144x6144 by default). All generated in C++ — no
image library beyond the vendored CImg, no external tools.

| variable | effect |
| --- | --- |
| `KTPLACE_ANIM` | enable the animation |
| `KTPLACE_ANIM_MAX_FRAMES` | frame budget for the run (default 1200) |
| `KTPLACE_ANIM_ZOOM` | frame scale, vs 768x768 (default 2) |
| `KTPLACE_ANIM_BLEND` | in-between frames per placement (default 3) |
| `KTPLACE_ANIM_DELAY_CS` | GIF frame delay, in hundredths of a second |
| `KTPLACE_FINAL_ZOOM` | final still scale, vs 768x768 (default 8) |
| `KTPLACE_FINAL_PPM` | also write the lossless PPM beside the PNG (off) |

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
