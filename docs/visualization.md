# Visualization: how a run's pictures are made

This note describes how the placement stages hand snapshots to the visualizer, what the flow
does with them, and how a run can record its snapshots and have `ktplace-viz` draw them
afterwards. It covers the refactor that separated the two (PR #16 and the trace work that
follows it), so it also says what changed and why.

## Why it changed

Two problems, measured on adaptec1:

- **Drawing was on the solver's critical path.** Every SVG frame, every raster for the GIF and
  the 6144x6144 final still were written synchronously, inside the placer's loops. A full run
  with pictures took 124 s in the dev container and wrote about 1.2 GB under `plots/`; the
  same run recording its snapshots instead takes 26 s (see below).
- **The stages knew about the visualizer.** Each stage's `build*DM` bound its frame hook to the
  `Visualizer` singleton; SimPL asked the visualizer whether it was animating, wrote
  `simpl_bounds.csv`/`.svg` and built density-map SVG text itself, and used
  `constraintMgr::kNoRegion`, which it only saw through the visualizer header; NTUPlace1 called
  `Visualizer::instance()` directly. A placer could not be built or tested without the
  renderer, and the renderer could not be run without the placers.

The goal is modules with as few dependencies as possible: a stage computes, the flow wires, the
visualizer draws.

## Layering

```
  stage (SimPL, NTUPlace1,          flow (src/kt_flowMgr.cc)            sink
  Abacus, multi-row, FastDP)
  ───────────────────────          ────────────────────────           ──────────────────────
  says *when* a snapshot     ──▶   begins each stage, installs  ──▶   FrameSink
  matters, hands over the          the hooks, converts each             ├─ Visualizer
  numbers through a hook           stage's FrameInfo into a             │   draws now: SVG,
  (onFrame, onDensity,             VisualFrame / DensityField           │   raster, GIF, PNG
  onBounds)                                                             └─ TraceWriter
                                                                            records to
  knows nothing of files,                                                   plots/trace.ktrace
  formats or the visualizer                                                      │
                                                                                 ▼
                                                                    ktplace-viz replays the
                                                                    trace into a Visualizer
```

After the refactor, `src/kt_flowMgr.cc` is the only file outside `src/visualization/` that
includes a visualization header. `ktplace-viz` links only `visualization`, `datamodel`,
`constraint` and `util` (see `VIZ_DIRS` in `src/Master.make`), so a visualization object that
starts depending on a placer stops that binary linking.

## Stages: hooks, not drawing

Each stage's design struct carries hooks. A null hook means nothing is wanted, and the stage
then builds no snapshot at all.

| stage | hook(s) | payload |
| --- | --- | --- |
| SimPL (`placer/simpl/simpl_design.h`) | `onFrame`, `onDensity`, `onBounds` | positions + `FrameInfo`; `DensityInfo` (grid, `occ`, `avail`, blockages); `BoundsPoint` per iteration |
| NTUPlace1 (`placer/ntuplace1/kt_ntuplace1.h`) | `RatioPlacer::setFrameHook` | positions + `RatioFrameInfo` |
| Abacus (`legalizer/abacus/abacus_design.h`) | `onFrame` | positions + `FrameInfo` |
| multi-row (`legalizer/multirow/multirow_design.h`) | `onFrame` | positions + `FrameInfo` |
| FastDP (`detailPlacer/fastdp/fastdp_design.h`) | `onFrame` | positions + `FrameInfo` |

Rules a stage follows:

- It hands over **numbers, never pictures or paths**. SimPL's density map used to be SVG text
  built in the solver; it is now the bin grid, and `Visualizer::densityMap` draws it. The
  bounds curve used to be two files SimPL wrote; it is now a vector, and
  `Visualizer::boundsCurve` writes them.
- Its `build*DM` installs **no** hooks. What a run draws is the flow's decision.
- It decides **how often** to offer a snapshot, because it knows how long its loops are. That
  cadence is the stage's own option (below), not a picture setting.

## The flow: the one place they meet

`FlowMgr` (`src/kt_flowMgr.cc`):

1. Picks the sink: `Visualizer::instance()`, or a `TraceWriter` when the run was started with
   `--record`.
2. Before each stage, if the sink is enabled, calls `beginStage` (`simpl`, `legalize`,
   `legalize_multi`, `detailplace`) and installs the stage's hooks. With `--no-plots` no hook is
   installed, so no snapshot is built.
3. Converts each stage's `FrameInfo` into a `VisualFrame` through one adapter,
   `drawFrames<Info>`. Every stage's `FrameInfo` has the same fields; the duplication is
   deliberate, so no stage includes a visualization header.
4. Applies the one cadence rule that depends on the sink: SimPL's warm-up CG frames are off
   unless the run animates. Setting `simpl cg_every_init` overrides it, because SimPL applies
   its options over the parameters it is given.
5. At the end, calls `finalImage` and `finish` (the GIF) on the sink.

### The sink interface

`visualization/kt_frameSink.h` holds the snapshot types (`VisualFrame`, `StageVisual`,
`DensityField`, `BoundsSample`) and `FrameSink`, the calls the flow makes, in the order it makes
them:

```cpp
beginStage(StageVisual)                       // a stage starts offering snapshots
frame(graph, x, y, die, fences, VisualFrame)  // one placement snapshot
densityMap(DensityField)                      // a bin-density field
boundsCurve(name, title, curve)               // a placer's two bounds per iteration
holdBack(frames)                              // reserve GIF budget for later stages
finalImage(graph, fences, hpwl)               // the committed placement, high resolution
finish(graph, x, y, die, fences, hpwl)        // close the animation
enabled(), animating()
```

`Visualizer` implements it by drawing. `TraceWriter` implements it by recording.

## Options

The picture settings (`PlotOptions`, read from `add_option visualizer ...`) only decide what a
snapshot becomes: animation on or off, frame budget, zoom, final still size. How often a stage
offers a snapshot is that stage's option:

| option | default | meaning |
| --- | ---: | --- |
| `simpl trace_every` | 1 | bounds (LSS and LAL frames) every N global iterations; 0 = none |
| `simpl cg_every` | 0 | a frame every N CG iterations inside a global solve |
| `simpl cg_every_init` | 1 | the same inside the warm-up solves; off unless animating, unless set |
| `simpl density_maps` | 1 | a density map beside every placement frame |
| `abacus frame_every` | 0 | a frame every N cells placed, between the always-drawn first and last |
| `multirow frame_every` | 20000 | a frame every N cells placed |

FastDP offers one frame per pass and has no cadence to set. The defaults are the values the
flow used to pass in, so a run draws what it drew before the refactor.

> These are set with `add_option` in the Tcl script a run is given with `ktplace -t` (the
> flag arrives with PR #15). A build without that flag has no way to load a script, so there
> these options keep their defaults.

## Recording a run, drawing it later

```sh
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1 --record
ktplace-viz output/adaptec1/plots/trace.ktrace
```

`--record` writes `<work-dir>/plots/trace.ktrace` and no pictures. `ktplace-viz` draws the trace
into the trace's own directory by default, which gives the layout the run would have drawn
(`simpl/`, `legalize_*.svg`, `detailplace_*.svg`, `simpl_bounds.*`, `anim/placement.gif`,
`final/`).

| `ktplace-viz` option | effect |
| --- | --- |
| `-o, --out <dir>` | draw somewhere else |
| `--no-anim` | frames and stills only, no GIF |
| `--final-zoom <z>` | another size for the final still |
| `-v` | also echo the trace log |

The picture settings default to the ones the run recorded. A run can be drawn any number of
times without placing it again.

Measured on adaptec1 (SimPL, Abacus, FastDP):

| | wall time | disk |
| --- | ---: | ---: |
| draw during the run | 124 s | ~1.2 GB |
| `--record` | 26 s | 220 MB trace |
| `ktplace-viz` afterwards | 81 s | the same pictures |

### Guarantee: the replay draws exactly what the run would have

A replay makes the visualizer the same calls, in the same order, with the same values: floats
and doubles are stored as their raw bytes, not printed. So the pictures a trace draws are
**byte-identical** to the ones the run would have drawn. This is checked by:

- `test_flow` `a_recorded_run_draws_nothing_and_its_trace_draws_the_same_pictures`, which
  compares every file of a recorded-then-drawn run with a drawn one;
- the CI step *A recorded run draws the same pictures*, which does the same on ibm01 with
  `diff -r`;
- by hand, before merging: ibm01 with SimPL (136 files) and NTUPlace1 (13), and adaptec1 (249),
  all byte-identical, including the GIF and `final.png`.

### Trace format

gzip at level 1 (the solver waits on it, and coordinates gain little from more), holding:

```
"KTTRACE\0"  u32 version (kTraceVersion, currently 1)
records: a one-byte tag, then its payload
  'O'  picture settings (PlotOptions, without the directory)
  'N'  cell geometry: width, height, x, y (f64 arrays), flags (bit0 fixed, bit1 terminal)
  'R'  fence regions: name, rectangles (lo.x lo.y hi.x hi.y)
  'S'  stage begins: stage, subdir, cadence
  'F'  frame: fences flag, die, VisualFrame fields, x and y (f32 arrays)
  'D'  density field: series, step, note, overflow, die, grid, occ, avail, blockages
  'B'  bounds curve: name, title, (upper, lower) pairs
  'H'  frames held back
  'I'  final image: fences flag, hpwl, committed x and y (f64 arrays)
  'E'  animation closes: fences flag, die, hpwl, x and y (f32 arrays)
  'Z'  end of trace
```

Little-endian fixed-width fields; a string is a u32 length and its bytes, an array a u64 count
and its elements. The netlist is recorded once, at the first snapshot; names, nets and region
ids are not recorded because nothing draws them.

A trace without `'Z'` is from a run that stopped early: `ktplace-viz` draws what it has and says
so. A file cut off part-way through a record (a fatal error exits without closing the trace) is
drawn up to the cut. A file that is not a trace, or is from another version, is refused.

## Adding a stage that draws

1. Give the stage's design struct a hook: a `FrameInfo` with the fields above, and
   `std::function<void(xs, ys, info)>`. Call it only when it is set.
2. Make the cadence an option of the stage's own (`<stage>_options.{h,cc}`), applied where the
   stage assembles its parameters.
3. In `FlowMgr`, call `sink->beginStage({...})` and install
   `drawFrames<stage::FrameInfo>(*sink, *db, die)`, only when `sink->enabled()`.
4. Do not include anything from `visualization/` in the stage.

A new kind of picture (not a placement frame) gets a struct in `kt_frameSink.h`, a `FrameSink`
call, a drawing in `Visualizer`, and a record in the trace (a new tag and a version bump).

## Known gaps

- **NTUPlace1's frames are not drawn.** Its hook exists, but the flow does not install it: no
  stage was ever begun for it, so before the refactor the visualizer dropped every snapshot
  (535 per run on ibm01). Drawing them adds about one frame per cut and changes the GIF, so it
  is left as its own decision.
- **The trace stores every cell, in full precision.** Positions of fixed cells repeat in every
  frame, and 16-bit die-relative coordinates would be lossless at the largest picture size
  (6144 px needs 13 bits). Either would shrink the trace several times, at the cost of the
  byte-identical guarantee being argued rather than exact.
- **Recording is still on the solver's thread.** A background writer would take the
  compression off it too.
- **Holding back GIF frames is recorded as a count.** A replay with another frame budget keeps
  the run's reservation, which `Visualizer::holdBack` clamps.
- **The web console's liveness check counts SVG frames** (`webui/server.py`). A recorded run
  draws none, so the console has only CPU use to go on; the trace's growing size would be a
  better signal.
