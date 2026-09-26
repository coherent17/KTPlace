# KTPlace
An open-source know thyself placement engine.

## Build

Requirements: g++ 13+ (C++23), oneTBB, Boost.Iostreams, zlib, libfmt (`libfmt-dev`).

```sh
make            # builds build/bin/ktplace
make rebuild    # clean + rebuild
```

The top-level Makefile drives a hierarchy under `src/`, where `src/Master.make`
coordinates the components -- `datamodel`, `adaptor`, `placer`, `visualization`
and `util` -- each of which builds through its own `Master.make`.

Layout:

```
src/
  Master.make          # coordinates the subdirectories
  kt_place.cc          # entry point
  kt_flowMgr.{h,cc}    # load -> place -> write flow
  kt_option.{h,cc}     # command line
  datamodel/           # PlacementDB, Graph, Vertex, Edge
  adaptor/             # Bookshelf and LEF/DEF readers
  placer/              # quadratic placer (clique/star, CG, density)
  visualization/       # SVG frames, HPWL curve, HTML gallery
  util/                # kt_log, kt_timer
```

## Code style

All C++ sources are formatted with `clang-format` (configuration in
`.clang-format`, tuned to the project's 4-space / 100-column style):

```sh
clang-format -i src/**/*.cc src/**/*.h
```

## Logging

All output goes through a single logger (`src/util/kt_log.h`) to a transcript
file plus stderr; **stdout is never written to**, so redirecting it stays clean.

```sh
./build/bin/ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl -l run.log
./build/bin/ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl -v   # + trace
```

| call | goes to log file | goes to stderr |
|------|:----------------:|:--------------:|
| `ktlog.echo(...)`  | yes | yes |
| `ktlog.trace(...)` | yes (only with `-v`) | no |
| `ktlog.fatal(...)` | yes | yes, then `exit(1)` |

Messages are built with `fmt::format` and checked at compile time.

## Benchmarks

Benchmarks are not vendored (they are several GB). Fetch them from their
original publishers:

```sh
python3 benchmark/fetch_benchmarks.py            # everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

| Directory | Designs | Source |
| --- | --- | --- |
| `ISPD_2015_raw/` | 16 `mgc_*` LEF/DEF designs | ispd.cc contest site |
| `ICCAD04/` | ibm01-18 (IBM-MSwPins), dma, dsp1/2, risc1/2 (Faraday) | vlsicad.eecs.umich.edu |
| `ISPD02/` | ibm01-18 (IBM-MS) | vlsicad.eecs.umich.edu |
| `ISPD06/` | adaptec*, bigblue*, newblue* | no official download remains |

See [benchmark/README.md](benchmark/README.md) for details.

## Usage

```sh
./build/bin/ktplace <name> <input_dir> <output.pl> [options]
./build/bin/ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl
```

The input format is auto-detected: a directory containing a `.def`/`.def.gz`
goes through the LEF/DEF adapter, everything else is loaded as Bookshelf.

| option | meaning |
| --- | --- |
| `-a, --algorithm <name>` | placement algorithm (default `quadratic`) |
| `-f, --format <fmt>` | output format (default `bookshelf`) |
| `-l, --log <file>` | transcript log file (default `ktplace.log`) |
| `-v, --verbose` | trace-level diagnostics into the log |
| `-p, --plot <dir>` | SVG frames + HPWL curve + HTML gallery |
| `-c, --config <file>` | configuration file |
| `-h, --help` / `-V, --version` | help / version |

### Visualizing the solve

Pass `-p <dir>` (or `--plot`) to emit SVG snapshots of the placement at each
outer iteration, an HPWL/overflow curve (`hpwl.csv` + `hpwl.svg`), and an HTML
gallery; open `<dir>/index.html` in a browser.

```sh
./build/bin/ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl -p ./output/plots
```

Frames are decimated above 150k cells, so even million-cell designs render fast.
Plotting evaluates HPWL after every recorded iteration, which adds a netlist
pass per frame (visible in the solve time).

The yellow dashed curve (and the `overflow` column of `hpwl.csv`) reports the
**density overflow**: the fraction of movable-cell area sitting in bins that
exceed a full 64x-bin capacity. 0.0 means the die is uniformly covered; 1.0
means everything is stacked in a single bin. `ktplace` spreads cells with a
SimPL-style projection: a gated equi-area fill drains over-packed bins into
empty die area, so the collapsed center seed never shows up as a pile of cells
in a corner.

## Features

- Bookshelf format input (`.nodes`, `.nets`, `.pl`, `.scl`, `.wts`), transparent `.gz` support via Boost.Iostreams
- LEF/DEF input (contest style: `floorplan.def` + `cells.lef`/`tech.lef`): macro sizes/pins, die area, rows, fixed macros, I/O pads, and the flat DEF netlist; DEF micron units respected (LEF sizes auto-scaled)
- Gzip + node/net parsing parallelized with oneTBB (`tbb::parallel_for`)
- Quadratic placement: clique/star-hybrid net model, CSR matrix, Jacobi-preconditioned CG, all parallelized with oneTBB
- Bookshelf `.pl` output writer
- Density-aware global placement: 64x64 occupancy grid over the die, SimPL-style projection spreading (gated equi-area drain) followed by optional wirelength refinement
- Iteration-by-iteration placement visualization (SVG frames, HPWL/overflow curve, gallery) — all generated in C++, no image libraries

## Timing

Elapsed time is measured with `src/util/kt_timer.h`: `ScopedTimer` is an RAII
stopwatch that records on scope exit, and `TimerRegistry` accumulates named
totals (with min/max and call counts) that are reported once per run:

```cpp
{
    ScopedTimer timer("load");
    runLoad();
}                        // recorded here
TimerRegistry::instance().report();
```

This is measurement only. Circuit delay (cell delay, net delay, slack) needs a
timing graph and cell libraries, so it belongs in a separate timing engine --
which can use `ScopedTimer` to report its own cost.

## Documentation

- [Benchmarks](benchmark/README.md) - suites, sources and layout

## Notes

Pure quadratic placement minimizes *squared* wirelength; without a spreading
step every cell slides to a single point (the netlist's force-balance point),
so KTPlace couples the wirelength solve to a density-aware projection-spreading
pass. Many Bookshelf `.pl` seeds are degenerate (every cell
at the origin), so movable cells are re-seeded at the center of the die before
spreading; the spread HPWL is not comparable to the seed value — legalization
is the standard next stage to compress wirelength again.
