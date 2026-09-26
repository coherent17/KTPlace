# KTPlace
An open-source know thyself placement engine.

## Build

Requirements: g++ 13+ (C++23), oneTBB, Boost.Iostreams, zlib, libfmt (`libfmt-dev`).

```sh
make            # builds build/bin/ktplace
make rebuild    # clean + rebuild
```

A successful build also writes two environment helpers into the repository
root. Source the one for your shell and the engine is callable by name:

```sh
source ktplace.sh     # bash / sh
source ktplace.csh    # csh / tcsh
ktplace ibm01 ./benchmark/ICCAD04/ibm01 ./output/ibm01.pl
```

They only appear when the link succeeds, are safe to source repeatedly, and
export `KTPLACE_HOME` pointing at the repository. The maintained copies live in
`scripts/`.

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
  util/                # kt_log, kt_reportTable, kt_scopedTimer
```

## Tests

Unit tests use Boost.Test and live next to the code they cover:

```
src/datamodel/test/test_datamodel.cc   PlacementDB and the placement graph
src/adaptor/test/test_adaptor.cc      Bookshelf and LEF/DEF readers
src/test/test_flow.cc                 end-to-end load -> place -> write
```

```sh
make test        # build and run every suite (alias: make check)
```

Each test writes its own tiny synthetic design into a scratch directory, so the
suites need no benchmark data. Error paths that end the process through
`ktlog::fatal` are checked with `fork(2)`, since Boost.Test 1.83 has no
death-test macros.

CI (`.github/workflows/ci.yml`) runs on every push and pull request: it
installs the dependencies, rejects any source that is not `clang-format` clean,
builds, runs the unit tests, then fetches one small benchmark (ICCAD04 `dma`,
~8 MiB) and asserts the placement completes and writes one record per cell.

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

| call | `ktplace.log` | `<log>_trace.log` | stderr |
|------|:--------------:|:-----------------:|:------:|
| `ktlog.echo(...)`  | yes | no | yes |
| `ktlog.trace(...)` | no  | yes (only with `-v`) | no |
| `ktlog.fatal(...)` | yes | no | yes, then `exit(1)` |

Trace records go to a **separate** file so the main transcript stays readable,
and that file is not created at all without `-v`. Messages are built with
`fmt::format` and checked at compile time.

`ktReportTable` (`src/util/kt_reportTable.h`) accumulates cells and renders an
aligned table — column widths measured from content, numeric cells
right-aligned — emitted as a single log record:

```cpp
ktReportTable table("Summary");
table.setHeaders({"phase", "wall", "cpu"});
table.addRow({"load", "4.75s", "7.09s"});
table.emit();
```

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
| `ISPD_2015/` | 16 `mgc_*` LEF/DEF designs | ispd.cc contest site |
| `ICCAD04/` | ibm01-18 (IBM-MSwPins), dma, dsp1/2, risc1/2 (Faraday) | vlsicad.eecs.umich.edu |
| `ISPD02/` | ibm01-18 (IBM-MS) | vlsicad.eecs.umich.edu |

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
| `-l, --log <file>` | transcript log (default `ktplace.log`) |
| `-v, --verbose` | also write `<log>_trace.log` |
| `-p, --plot <dir>` | SVG frames + HPWL curve + HTML gallery |
| `-w, --work-dir <dir>` | base for relative output/plot/log paths |
| `-c, --config <file>` | configuration file |
| `-h, --help` / `-V, --version` | help / version |

With `-w`, relative `output_path` and plot directories are resolved under that
directory (absolute paths are used verbatim), the logs default to
`<work-dir>/ktplace.log` and `<work-dir>/ktplace_trace.log`, and the directory
is created if missing. Without it, behaviour is unchanged and logs land in the
current directory.

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

Elapsed time is measured with `src/util/kt_scopedTimer.h`: `ScopedTimer` is an RAII
stopwatch that records on scope exit, and `TimerRegistry` accumulates named
totals that are reported once per run. Every interval is recorded as both
wall-clock and processor time, so the table shows how much parallelism a phase
actually used:

```
Timings (wall 15.383s, cpu 53.081s, 3.45x parallelism)
+-------+---------+---------+-------+----------+
| phase | wall    | cpu     | calls | cpu/wall |
+-------+---------+---------+-------+----------+
| load  | 4.752s  | 7.086s  |     1 |    1.49x |
| place | 10.338s | 45.701s |     1 |    4.42x |
| write | 0.293s  | 0.293s  |     1 |    1.00x |
+-------+---------+---------+-------+----------+
```


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
- [SimPL and this placer](docs/simpl.md) - what SimPL does, what we do differently, and a known defect in the phase schedule

## Notes

Pure quadratic placement minimizes *squared* wirelength; without a spreading
step every cell slides to a single point (the netlist's force-balance point),
so KTPlace couples the wirelength solve to a density-aware projection-spreading
pass, followed by a refinement phase that pulls the spread placement back toward
wirelength optimum. The consequence is visible in the reported `HPWL / seed`
ratio: spreading raises wirelength well above the seed, and there is no
legalization stage yet to bring it back down.

| design | cells | seed HPWL | after spreading | ratio |
| --- | ---: | ---: | ---: | ---: |
| `adaptec2` (ISPD 2005) | 255,023 | 7.27e7 | 1.32e9 | 18.2x |
| `adaptec5` (ISPD 2006) | 843,128 | 2.17e8 | 6.71e9 | 31.0x |
| `mgc_superblue16_a` (ISPD 2015) | 698,367 | 3.59e10 | 3.35e11 | 9.3x |
| `dma` (ICCAD 2004) | 11,734 | 0 | 6.6e3 | degenerate seed |

Two different things are being compared in that table, and it matters when
reading it:

- **A real seed.** ISPD 2005/2006 and ISPD 2002 ship a legal placement in
  `.pl`; the ratio is then a meaningful "how much did spreading cost" figure.
  Published placers are normally within 1.05-1.3x of such a seed because they
  finish with legalization and detail placement, so a 20-50x ratio means this
  is a post-spread, pre-legalization snapshot rather than a competitive result.
- **Our own seed.** ISPD 2015 LEF/DEF leaves standard cells `UNPLACED`, so the
  seed HPWL is measured from the die-center seed KTPlace invents, not from the
  input. Comparing against it says nothing about input quality.
- **A degenerate seed.** Some Bookshelf suites place every cell on the origin
  (ICCAD 2004 `dma` above), where any percentage is meaningless and is reported
  as `n/a`.
