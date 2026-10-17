# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

Plain GNU make, C++23 with g++ 13+. oneTBB, zlib and CImg are git submodules under `third_party/` (clone with `--recurse-submodules`); never edit them. `src/Master.make` compiles oneTBB and zlib into `build/lib/libtbb.a` and `libz.a` and links them statically; CImg is header-only and included as `"CImg.h"`. Formatting uses `std::format`, not fmt. The only system packages are Boost.Test (unit tests) and clang-format.

```sh
make -j"$(nproc)"          # build/bin/ktplace, and copies ktplace.sh/.csh to the repo root
make -j"$(nproc)" test     # build and run every Boost.Test suite (alias: make check)
make format                # clang-format -i on every tracked src/*.cc, src/*.h
make format-check          # what CI checks; it fails on any unformatted file
make compile-commands      # compile_commands.json for clangd
make hooks                 # pre-commit: staged-file format check plus unit tests
                           #   (KTPLACE_SKIP_HOOKS=1 bypasses it; KTPLACE_HOOK_TESTS=0 skips the tests)
python3 scripts/coverage_report.py --clean --build --min 80   # gcov line coverage (local only, not in CI)
make STATIC=1           # build-static/bin/ktplace with glibc and libstdc++ linked in too
```

Each component has its own test binary: `test_datamodel`, `test_adaptor`, `test_flow`, `test_util`, `test_constraint`, `test_option`, `test_viz`, `test_detail`, `test_ntuplace1`, `test_abacus`, `test_tcl` and `test_simpl`. To run one suite or one case, build the tests and then call Boost.Test directly:

```sh
make -C src -f Master.make ../build/bin/test_abacus   # path is relative to src/
build/bin/test_abacus --log_level=test_suite
build/bin/test_abacus --run_test=ktplace_abacus/<case>   # --list_content lists them
```

A new test binary needs a `TEST_<name>` variable, a `TEST_BINS` entry and a link rule in `src/Master.make`. A new source file must be added to `SRCS` in its directory's `Master.make`. The build does not glob.

CI (`.github/workflows/ci.yml`) has a `build` job (format check, build, test binaries; it uploads the stripped `build/bin` as an artifact) and four jobs that start after it and run in parallel: unit tests, the adaptec1 smoke placement, the ibm01 multi-row smoke placement (each configured by a small `-tcl` script), and a determinism job that runs ibm01 (animation on) and adaptec1 twice and requires byte-identical outputs. A `macos` job builds, tests and places ibm01 and adaptec1 with Apple clang, and a last job requires the Linux and macOS `placed.pl` files to be byte-identical. The smoke step checks the exact netlist counts in the log and the line count of `placed.pl`. Do not change those log strings by accident.

Releases come from `.github/workflows/release.yml`: pushing a `v*` tag that matches `version` in `src/kt_option.h` builds a dynamic and a fully static binary, places ibm01 and adaptec1 with each, and publishes both tarballs with checksums as a GitHub release.

## Running

```sh
source ktplace.sh
ktplace benchmark/bookshelf/ICCAD04/ibm01 -w output/ibm01 [-tcl run.tcl] [-v]
```

`ibm01` is small and the only design with multi-row cells. `adaptec1` is large. The suites are committed gzipped under `benchmark/bookshelf/` and `benchmark/lefdef/`, except the ISPD 2006 designs; `python3 benchmark/fetch_benchmarks.py` downloads any of them. The ISPD 2005 contest evaluator is in `benchmark/bookshelf/ISPD_2005/evaluator/` (`evaluate.sh <design dir> <placed.pl>`). Everything a run writes goes under `-w`: `placed.pl`, `plots/`, `ktplace.log` and `ktplace_trace.log`. **stdout is never written.** Logging goes through `ktlog` (`util/kt_log.h`) to the log file and stderr; `ktplace_trace.log` gets every level (trace, echo, warning, fatal). A stage's `build<Stage>DM` reports what it copied and its options through `reportStageBuild` (`datamodel/kt_stageReport.h`), the flow prints `ktDM::checksum()` after each stage commits, and `ScopedTimer(name, true)` announces a stage's time when it ends.

## Configuration: Tcl option store

Run configuration goes through a Tcl script passed with `-tcl` (the README's *Configuration* section lists every key):

```tcl
add_option flow algorithm ntuplace1   # simpl is the default
add_option simpl grid 64
add_option visualizer anim 0
```

- `datamodel/kt_optionDb.h`: `OptionDb` is a process-wide store of `Component -> key -> std::variant<int,bool,double,string>`. Valid components are the `Component` enum and its `componentNames` (`simpl`, `ntuplace1`, `abacus`, `multirow`, `fastdp`, `visualizer`, `flow`), and `add_option` rejects any other name. A new component needs an enumerator and a name.
- Each stage has one `<stage>_options.h` struct (`SimplOptions`, `AbacusOptions`, ...) that is the stage's whole configuration: its field initializers are the defaults, `component` names its component, and a static `fields(self, visit)` lists only the keys a script may set. Constants nobody should change are plain members outside `fields()`. There are no separate `*Params` structs: the algorithm takes the options struct.
- `build<Stage>DM` calls `loadOptions<XOptions>()`, which overwrites each listed field the script set, and stores the struct in the DM. The DM's `run()` reports it with `reportOptions(opts)` and hands it to the algorithm. Nothing keeps the `ComponentOptions` that `OptionDb::getComponentOption` returns.
- Lookup order: the script value, then the default. **The environment is not read.** `ComponentOptions::get<T>(key)` returns `std::optional<T>`; a value of the wrong type is not converted: it logs a warning and the default is used. A misspelled key is stored and never read.
- `tcl/` holds a small Tcl subset (`set`, `expr`, `if`, `foreach`, `source`, ...) and `TclMgr`, which adds `add_option`, the only ktplace-specific command. `FlowMgr::run` loads the script in its first step, `parseTcl`, before anything reads the store.

## Architecture

`kt_place.cc` (main) → `kt_option` (CLI) → `FlowMgr::run` (`kt_flowMgr.cc`), which is a fixed pipeline:

load → report → global place → legalize → detailed place → final image, GIF → write `.pl`

- **Input** (`adaptor/`): `makeInputReader` auto-detects the format. A `.def`/`.def.gz` goes to the LEF/DEF adaptor and anything else goes to Bookshelf. Both build the shared `ktDM`.
- **Shared database** (`datamodel/`): `ktDM` owns the `Graph`, where cells and nets share one vertex-id space, along with the die and rows, the constraints (fences, `datamodel/kt_constraintMgr`) and design-level queries such as `multiRowCells()`, `hpwl()`, the placement check and `fencedSubrows()`: the rows cut at fence boundaries, each piece labelled with its region (`datamodel/kt_rowFence.h`). The row-based stages get those labelled subrows from their DM builder and never split rows by fences themselves.
- **Stage shape.** A stage owns a DM, and everything it needs lives in that DM. `<stage>::flowMgr` drives it:
  1. Its constructor calls `build<Stage>DM(ktDM)`, which converts ktDM into the stage's own data model (`<stage>_dm.h`: the design, its fences, its result type).
  2. The same call loads the stage's options from `OptionDb` (`loadOptions<XOptions>()`) into the DM.
  3. `flowMgr::run()` runs the algorithm, which modifies the DM's own design (the cells' positions); it returns only the run statistics.
  4. `flowMgr::solution()` hands the DM's positions to the flow, which commits them with `ktDM::set<Stage>Solution` (`setSimplSolution`, ...). That is the only place the shared design is written, and the flow then runs the legality check and reports the checksum.

  Every stage follows this shape: `simpl`, `ntuplace1`, `abacus`, `multirow` and `fastdp`, committed with `setSimplSolution`, `setNtuplace1Solution`, `setAbacusSolution`, `setMultiRowSolution` and `setFastdpSolution`. A stage never reads ktDM after building its DM.

  SimPL's files: `simpl_dm` (data model: design, fences, result, `buildSimplDM`), `simpl_flowMgr` (the four steps), `simpl_placer` (netlist and the iteration loop), `simpl_lss` (linear system solve: B2B and star systems, conjugate gradient), `simpl_lal` (look-ahead legalization), `simpl_grid` (bin grid and free space), `simpl_parallel.h` (TBB helpers with deterministic reductions), `simpl_graph`, `simpl_options`.
- **Stages:**
  - `globalPlacer/simpl`: default quadratic placer with B2B net model and look-ahead legalization, fence-aware. `docs/simpl.md` explains how it differs from the published SimPL.
  - `globalPlacer/ntuplace1`: alternative global placer, ratio partitioning (Chen, Hsu, Jiang, Chang, ISPD 2005).
  - `legalizer/abacus`: single-row legalizer, fence-aware.
  - `legalizer/multirow`: used instead of Abacus when the design has cells taller than one row.
  - `detailPlacer/fastdp`: detailed placement, following Pan, Viswanathan and Chu (ICCAD 2005): single-segment clustering, then global swap, vertical swap and 3-cell local re-ordering until the gain is small, then clustering again.
- **Visualization** (`visualization/`): stages never include it. A stage reports snapshots through plain-data hooks on its own `Design` (`onFrame`; SimPL also `onBounds`), and `FlowMgr` installs them (`recordStage` in `kt_flowMgr.cc`) only when plots are on. The hooks append to a `SnapshotWriter` (`plots/snapshots.ktsnap`); after `placed.pl` is written, `drawPictures()` replays it into the `Visualizer` and deletes it, so nothing is drawn inside a stage's loops (`docs/visualization.md`). How often a stage offers a snapshot is that stage's own option (`simpl trace_every`, `abacus frame_every`, ...). `Visualizer::instance()` is the singleton the flow draws through, so all stages add to one GIF with one frame budget. Global placement may use at most 3/5 of the frames (`holdBack`). Everything is rendered in C++ (SVG, plus PNG/GIF through CImg), with no external tools.
- `util/`: logging, `KT_ASSERT`, `ScopedTimer`/`TimerRegistry`, report tables.
- **GUI** (`gui/`, optional, Qt 6): `ktplace --gui`, linked in only by `make GUI=1` (otherwise `kt_noGui.cc` is linked and `--gui` is fatal; `STATIC=1` refuses `GUI=1`); `make gui-test` runs its suite. `Runner` runs the unchanged `FlowMgr` on a worker thread with plots off and watches it through `FlowMgr::observe` (`FlowObserver`: `committed` after every commit, `frame` from each stage's frame hook). The layout geometry is copied once per run; frames carry only positions and are dropped while one is still queued. Stop throws out of the observer, and `ktlog.throwOnFatal(true)` turns a fatal into a `FatalError`, so neither ends the process. `ktlog.setSink` feeds the Log panel. Build with `QT_NO_KEYWORDS`: Qt's `emit` macro would rename `Logger::emit`.

## Conventions

- **Placements must not depend on the platform.** Linux (GCC, libstdc++) and macOS (Apple clang, libc++) must write the same `placed.pl`, and CI compares them. So in engine code: sort with `std::stable_sort`/`std::ranges::stable_sort` (an unstable sort orders ties differently per standard library), keep `-ffp-contract=off` (no FMA), avoid math functions that are not correctly rounded (`std::hypot`, `std::exp`, ...) where their result steers a decision, and keep parallel floating-point reductions in a fixed order (`deterministicSum` in SimPL).

- Includes are relative to the project root (`#include "datamodel/kt_dm.h"`). `src/` is the only include path.
- Format with `.clang-format`: Google-based style, 4-space indent, 100-column limit. CI fails on any unformatted tracked file.
- Top-level classes use PIMPL (`class Impl; std::unique_ptr<Impl> pImpl;`).
- Names: `kt_`-prefixed files and types (`ktDM`, `kt_option`) and per-stage namespaces (`ktplace::simpl`, `ktplace::abacus`, ...).
- **Identifier naming** (enforced by `.clang-tidy`, `readability-identifier-naming`): functions, methods, members, variables, parameters and constants are all `camelBack`, with no prefix or suffix: no trailing `_` on members, no `m_`, no `k` on constants (`noRow`, `version`, `componentNames`). Enum values are `CamelCase` (`Component::Simpl`, `Level::Warning`); prefer `enum class`. If the plain name collides with a method or a local, pick a more descriptive name (`cellX`, `regionList`) instead of decorating it.
- **Modern C++ and includes** (`.clang-tidy` turns on `modernize-*` and `misc-include-cleaner`; `.clangd` shows both in the editor and flags unused includes): range-for, `auto` for casts, `emplace_back`, `starts_with`, raw string literals. Include what a file uses and nothing it doesn't: a `<stage>_dm.h` includes only the options and design headers, and its `.cc` includes the algorithm header. Run `clang-tidy -p . <file>` after `make compile-commands`; a change should add no new findings.
- **Write clean, self-explanatory code without comments.** The existing code is heavily commented. Don't copy that style. Only fix a comment that has become wrong.
- **Don't add environment variables.** A new setting is an `Option<T>` on its component, set from a Tcl script.
- `.cursorrules` is outdated: it describes CMake, Eigen, GTest and an `include/` tree, none of which exist. Trust the code and the Makefiles over it.
