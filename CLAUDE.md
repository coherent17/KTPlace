# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and test

Plain GNU make, C++23 with g++ 13+. Dependencies are system packages: oneTBB, Boost (Iostreams, Test), zlib, fmt and clang-format. CImg is vendored as `src/visualization/CImg.h`; never format or edit it.

```sh
make -j"$(nproc)"          # build/bin/ktplace, and copies ktplace.sh/.csh to the repo root
make -j"$(nproc)" test     # build and run every Boost.Test suite (alias: make check)
make format                # clang-format -i on every tracked src/*.cc, src/*.h (except CImg.h)
make format-check          # what CI checks; it fails on any unformatted file
make compile-commands      # compile_commands.json for clangd
make hooks                 # pre-commit: staged-file format check plus unit tests
                           #   (KTPLACE_SKIP_HOOKS=1 bypasses it; KTPLACE_HOOK_TESTS=0 skips the tests)
python3 scripts/coverage_report.py --clean --build --min 80   # gcov line coverage (local only, not in CI)
```

Each component has its own test binary: `test_datamodel`, `test_adaptor`, `test_flow`, `test_util`, `test_constraint`, `test_option`, `test_viz`, `test_detail`, `test_ntuplace1`, `test_abacus`, `test_tcl` and `test_simpl`. To run one suite or one case, build the tests and then call Boost.Test directly:

```sh
make -C src -f Master.make ../build/bin/test_abacus   # path is relative to src/
build/bin/test_abacus --log_level=test_suite
build/bin/test_abacus --run_test=ktplace_abacus/<case>   # --list_content lists them
```

A new test binary needs a `TEST_<name>` variable, a `TEST_BINS` entry and a link rule in `src/Master.make`. A new source file must be added to `SRCS` in its directory's `Master.make`. The build does not glob.

CI (`.github/workflows/ci.yml`) has a `build` job (format check, build, test binaries; it uploads the stripped `build/bin` as an artifact) and three jobs that start after it and run in parallel: unit tests, the adaptec1 smoke placement, and the ibm01 multi-row smoke placement, each configured by a small `-tcl` script. The smoke step checks the exact netlist counts in the log and the line count of `placed.pl`. Do not change those log strings by accident.

## Running

```sh
source ktplace.sh
ktplace benchmark/ICCAD04/ibm01 -w output/ibm01 [-tcl run.tcl] [--no-plots] [-v]
```

`ibm01` is small and the only vendored design with multi-row cells. `adaptec1` is large. Fetch other benchmarks with `python3 benchmark/fetch_benchmarks.py`. Everything a run writes goes under `-w`: `placed.pl`, `plots/`, `ktplace.log` and `ktplace_trace.log`. **stdout is never written.** Logging goes through `ktlog` (`util/kt_log.h`) to the log file and stderr.

## Configuration: Tcl option store

Run configuration goes through a Tcl script passed with `-tcl` (the README's *Configuration* section lists every key). The web console (`webui/server.py`) still launches runs with `-a` and `KTPLACE_*` overrides, which the engine no longer accepts:

```tcl
add_option flow algorithm ntuplace1   # simpl is the default
add_option simpl grid 64
add_option visualizer anim 0
```

- `datamodel/kt_optionDb.h`: `OptionDb` is a process-wide store of `Component -> key -> std::variant<int,bool,double,string>`. Valid components are the `Component` enum and its `kComponentNames` (`simpl`, `ntuplace1`, `abacus`, `multirow`, `fastdp`, `visualizer`, `flow`), and `add_option` rejects any other name. A new component needs an enumerator and a name.
- Each stage has one `<stage>_options.h` struct (`SimplOptions`, `AbacusOptions`, ...) that is the stage's whole configuration: its field initializers are the defaults, `kComponent` names its component, and a static `fields(self, visit)` lists only the keys a script may set. Constants nobody should change are plain members outside `fields()`. There are no separate `*Params` structs: the algorithm takes the options struct.
- `build<Stage>DM` calls `loadOptions<XOptions>()`, which overwrites each listed field the script set, and stores the struct in the DM. The DM's `run()` reports it with `reportOptions(opts)` and hands it to the algorithm. Nothing keeps the `ComponentOptions` that `OptionDb::getComponentOption` returns.
- Lookup order: the script value, then the default. **The environment is not read.** `ComponentOptions::get<T>(key)` returns `std::optional<T>`; a value of the wrong type is not converted: it logs a warning and the default is used. A misspelled key is stored and never read.
- `tcl/` holds a small Tcl subset (`set`, `expr`, `if`, `foreach`, `source`, ...) and `TclMgr`, which adds `add_option`, the only ktplace-specific command. `FlowMgr::run` loads the script in its first step, `parseTcl`, before anything reads the store.

## Architecture

`kt_place.cc` (main) → `kt_option` (CLI) → `FlowMgr::run` (`kt_flowMgr.cc`), which is a fixed pipeline:

load → report → global place → legalize → detailed place → final image, GIF → write `.pl`

- **Input** (`adaptor/`): `makeInputReader` auto-detects the format. A `.def`/`.def.gz` goes to the LEF/DEF adaptor and anything else goes to Bookshelf. Both build the shared `ktDM`.
- **Shared database** (`datamodel/`): `ktDM` owns the `Graph`, where cells and nets share one vertex-id space, along with the die and rows, the constraints (fences, from `constraint/`) and design-level queries such as `multiRowCells()`, `hpwl()`, the placement check and `fencedSubrows()`: the rows cut at fence boundaries, each piece labelled with its region (`datamodel/kt_rowFence.h`). The row-based stages get those labelled subrows from their DM builder and never split rows by fences themselves.
- **Stage shape.** Every stage follows the same pattern:
  1. `build<Stage>DM(ktDM)` copies what the stage needs into its own `<stage>::Design` / `<stage>DM`. A stage never reads or writes `ktDM`.
  2. `<stage>::flowMgr` runs the algorithm on that DM and returns a `solutionMgr`, a value type holding positions indexed by vertex id.
  3. The top flow commits the result with `ktDM::set{GP,Legalization,MultiRow,DetailedPlace}Solution`. This is the only place the shared design is written, and it is where the legality check runs.

  The files follow the same layout in every stage: `<stage>_design`, `<stage>_dm`, `<stage>_flowMgr`, `<stage>_options` and `<stage>_placer`/`_legalizer`. This keeps stages testable without a `ktDM`.
- **Stages:**
  - `placer/simpl`: default quadratic placer with B2B net model and look-ahead legalization, fence-aware. `docs/simpl.md` explains how it differs from the published SimPL.
  - `placer/ntuplace1`: alternative global placer.
  - `legalizer/abacus`: single-row legalizer, fence-aware.
  - `legalizer/multirow`: used instead of Abacus when the design has cells taller than one row.
  - `detailPlacer/fastdp`: detailed placement.
- **Visualization** (`visualization/`): `Visualizer::instance()` is a singleton shared by every stage, so all stages add to one GIF with one frame budget. Global placement may use at most 3/5 of the frames (`holdBack`). Everything is rendered in C++ (SVG, plus PNG/GIF through CImg), with no external tools.
- `util/`: logging, `KT_ASSERT`, `ScopedTimer`/`TimerRegistry`, report tables.

## Conventions

- Includes are relative to the project root (`#include "datamodel/kt_dm.h"`). `src/` is the only include path.
- Format with `.clang-format`: Google-based style, 4-space indent, 100-column limit. CI fails on any unformatted tracked file.
- Top-level classes use PIMPL (`class Impl; std::unique_ptr<Impl> pImpl;`).
- Names: `kt_`-prefixed files and types (`ktDM`, `kt_option`) and per-stage namespaces (`ktplace::simpl`, `ktplace::abacus`, ...).
- **Write clean, self-explanatory code without comments.** The existing code is heavily commented. Don't copy that style. Only fix a comment that has become wrong.
- **Don't add environment variables.** A new setting is an `Option<T>` on its component, set from a Tcl script.
- `.cursorrules` is outdated: it describes CMake, Eigen, GTest and an `include/` tree, none of which exist. Trust the code and the Makefiles over it.
