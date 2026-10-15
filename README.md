# KTPlace
An open-source know thyself placement engine.

## Dependencies

g++ 13+ (C++23), oneTBB, Boost (Iostreams, Test), zlib, fmt, Tcl 8.6, clang-format.

```sh
sudo apt-get install -y --no-install-recommends \
    g++ libtbb-dev libboost-iostreams-dev libboost-test-dev zlib1g-dev libfmt-dev tcl-dev \
    clang-format
```

System packages, not submodules: every distribution ships these, and vendoring
them would only pin versions behind the distribution's own updates. CImg, which
is *not* packaged usefully, is vendored as a single header under CeCILL-C and
CeCILL; see the header in `src/visualization/` for its terms.

## Build

```sh
make            # builds build/bin/ktplace
make test       # build and run every unit suite
make gui        # build/bin/ktplace-gui, the Qt window (needs qt6-base-dev)
make gui-test   # its tests, headless
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

The desktop GUI (Qt 6): `ktplace --gui [design]` inside the dev container.
From the host, `scripts/devenv.sh gui [design]` shows it over VNC
(`vnc://127.0.0.1:5901`), or `scripts/devenv.sh x11 [design]` as a native window
on your X server (XQuartz on macOS); see
[docs/docker.md](docs/docker.md#5-the-desktop-gui). How the window drives the
same stages as the CLI, and on which threads, is in [docs/gui.md](docs/gui.md).

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
| `-tcl, --tcl <file>` | configure the run: a script of `add_option` lines, the placer included (`add_option flow algorithm ntuplace1`; simpl by default) |
| `-s, --script <file>` | run a Tcl flow script instead of the fixed flow (see below) |
| `--gui` | open the design in the Qt window instead; the other options go to it |
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

### Tcl scripts

`ktplace --script <flow.tcl> [input_dir] [options]` runs a script instead of the
fixed load / place / legalize / detail-place / write sequence. Each stage is a
command, so a script can reorder stages, run one alone, or change options
between them:

```tcl
add_option simpl density 0.9
read_design $ktplace(input_dir)
global_place -algo simpl
legalize
if {[llength [check_placement]] == 0} { report_hpwl }
detail_place
write_design $ktplace(output)
```

[scripts/flow.tcl](scripts/flow.tcl) is the fixed flow as a script; it writes the
same `placed.pl` and plots as running without `-t`, and CI checks that it does.
The script sees `ktplace(input_dir)`, `ktplace(work_dir)`, `ktplace(output)`,
`ktplace(plot_dir)` and `ktplace(algorithm)` from the command line.

| command | does |
| --- | --- |
| `read_design <dir>` / `report_design` / `write_design <path>` | load, describe, write the `.pl` |
| `global_place ?-algo name?` / `legalize` / `detail_place` | the stages |
| `report_hpwl` / `check_placement ?label?` / `report_timing` | queries; HPWL and the defect list are returned |
| `set_plot_dir <dir>` / `write_final_image` / `write_animation` | pictures (`""` draws nothing) |
| `add_option <component> <option> <value>` / `get_option` / `report_options` | the stages' options (see below) |
| `help ?command?` | the list above, from the shell |

A failing command is a Tcl error: `catch` it, or let it end the run with exit 1
and the file and line. Each top-level command is echoed to the log as
`tcl> ...`, so `ktplace.log` reads back as the script, and `puts` to stdout goes
to the log like everything else.

## Output

Under `<work-dir>/plots/`: SVG frames for the placement stages (`simpl/`, and
`legalize/` for the Abacus path), the detail placer (`detailplace/`), the density
bound trace (`simpl_bounds.csv`/`.svg`), the animation (`anim/placement.gif`,
unless `visualizer anim` is 0), and a high-resolution still of the finished
placement (`final/final.png`, 6144x6144 by default). All generated in C++ — no
image library beyond the vendored CImg, no external tools.

### Options

Every stage setting is an option of its component, set with
`add_option <component> <option> <value>` in a configuration script given with
`-tcl` (or in a `--script` script, or the GUI's console). The placer is one too:
`add_option flow algorithm ntuplace1` (SimPL by default). Nothing is read from the
environment, so what configured a run is in its script and in its log: each
stage prints its full option table when it runs, with what set each value.

```sh
cat > tuning.tcl <<'EOF'
add_option simpl iters 50
add_option simpl seed 7
add_option visualizer anim 0
EOF
ktplace benchmark/ISPD_2005/adaptec1 -tcl tuning.tcl -w output/adaptec1-50it
```

The components and their options are declared, with defaults, in
`src/placer/simpl/simpl_options.cc`, `src/placer/ntuplace1/ntuplace1_options.cc`,
`src/legalizer/abacus/abacus_options.cc`, `src/detailPlacer/fastdp/fastdp_options.cc`
and `src/visualization/visualizer_options.cc`. The drawing ones:

| `visualizer` option | effect |
| --- | --- |
| `anim` | record frames and assemble the GIF (default 1; `0` turns it off) |
| `anim_max_frames` | frame budget for the run (default 1200) |
| `anim_max_bytes` | size cap of the GIF, in bytes (default 96 MB) |
| `anim_zoom` | frame scale, vs 768x768 (default 2) |
| `anim_blend` | in-between frames per placement (default 3) |
| `anim_delay_cs` | GIF frame delay, in hundredths of a second (default 12) |
| `anim_final_hold` | extra frames the finished placement is held (default 8) |
| `anim_density_maps` | bin-density heat map beside each placement frame (default 1) |
| `anim_trace_every` | global-placement frame every N iterations (default 1; `0` keeps only the final one) |
| `anim_cg_every` | frame every N conjugate-gradient iterates (default 0, off) |
| `final_zoom` | final still scale, vs 768x768 (default 8) |
| `final_ppm` | also write the lossless PPM beside the PNG (default 0, off) |

On a large design the frames are most of global placement's time. When only the
placement matters, set `visualizer anim 0` and `visualizer anim_trace_every 0`
(the HPWL is the same), or pass `--no-plots` to skip every picture.

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
