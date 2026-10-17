# Visualization: how a run's pictures are made

This note describes how the placement stages hand snapshots to the flow, how the flow records
them while it places, and how it draws them once placement is done. It covers PR #16, which took
the visualizer out of the stages, and PR #17, which took the drawing out of the placement loops.

## Why

Two problems, measured on adaptec1:

- **Drawing was on the solver's critical path.** Every SVG frame and every raster for the GIF
  was written synchronously, inside the placer's loops, so global placement took 26 s of which
  most was drawing.
- **The stages knew about the visualizer.** Each stage's `build*DM` bound its frame hook to the
  `Visualizer` singleton; SimPL wrote `simpl_bounds.csv`/`.svg` itself; NTUPlace1 called
  `Visualizer::instance()` directly. A placer could not be built or tested without the
  renderer.

The goal is modules with as few dependencies as possible: a stage computes, the flow records,
the visualizer draws, and it draws after the placement is finished.

## Layering

```
  stage (SimPL, NTUPlace1,        flow (src/kt_flowMgr.cc)                 visualizer
  Abacus, multi-row, FastDP)
  ─────────────────────────       ──────────────────────────────           ───────────────
  says *when* a snapshot    ──▶   while placing: begins each stage
  matters, hands over the         and appends every snapshot to a
  numbers through a hook          SnapshotWriter (plots/snapshots.ktsnap)
  (onFrame, onBounds)
                                  after placed.pl is written:        ──▶   SVG frames,
  knows nothing of files,         replaySnapshots() into the Visualizer,   bounds curve,
  formats or the visualizer       then the final still and the GIF         final still, GIF
```

`src/kt_flowMgr.cc` is the only file outside `src/visualization/` that includes a visualization
header.

## Stages: hooks, not drawing

Each stage's design struct carries hooks. A null hook means nothing is wanted, and the stage
then builds no snapshot at all.

| stage | hook(s) | payload |
| --- | --- | --- |
| SimPL (`globalPlacer/simpl/simpl_design.h`) | `onFrame`, `onBounds` | positions + `FrameInfo`; `BoundsPoint` per iteration |
| NTUPlace1 (`globalPlacer/ntuplace1/kt_ntuplace1.h`) | `RatioPlacer::setFrameHook` | positions + `RatioFrameInfo` |
| Abacus (`legalizer/abacus/abacus_design.h`) | `onFrame` | positions + `FrameInfo` |
| multi-row (`legalizer/multirow/multirow_design.h`) | `onFrame` | positions + `FrameInfo` |
| FastDP (`detailPlacer/fastdp/fastdp_design.h`) | `onFrame` | positions + `FrameInfo` |

Rules a stage follows:

- It hands over **numbers, never pictures or paths**. SimPL's bounds curve is a vector, and
  `Visualizer::boundsCurve` writes the CSV and SVG.
- Its `build*DM` installs **no** hooks. What a run draws is the flow's decision.
- It decides **how often** to offer a snapshot, because it knows how long its loops are. That
  cadence is the stage's own option: `simpl trace_every`, `simpl cg_every`,
  `simpl init_cg_every`, `abacus frames`, `multirow frames`. Per-frame SVGs are written only
  with `visualizer svg_frames 1`; by default a frame goes only into the GIF.

## The flow

`FlowMgr` (`src/kt_flowMgr.cc`):

1. When plots are on, configures the `Visualizer` and opens a `SnapshotWriter` on
   `<work-dir>/plots/snapshots.ktsnap`. With `add_option visualizer plot 0` neither happens and no hook is installed,
   so no snapshot is built.
2. Before each stage, records `beginStage` (`simpl`, `legalize`, `legalize_multi`,
   `detailplace`) and installs the stage's frame hook through `recordStage()`, which converts
   the stage's `FrameInfo` into a `VisualFrame` and appends it to the snapshot file. Every stage's
   `FrameInfo` has the same fields; the duplication is deliberate, so no stage includes a
   visualization header.
3. Records the frame reservation for the later stages (`holdBack`) in the order the drawing
   needs it.
4. Writes `placed.pl`.
5. `drawPictures()`: closes the snapshot file, replays it into the `Visualizer`, draws the final still
   and closes the animation from the committed placement, then deletes the snapshot file
   unless `add_option visualizer keep_snapshots 1` asks to keep it. The `draw`
   row of the timing table is this step.

## The snapshot file

`visualization/kt_snapshots.{h,cc}`. gzip at level 1 (the solver waits on it, and coordinates gain
little from more), holding:

```
records: a one-byte tag, then its payload
  'S'  stage begins: stage, subdir, cadence
  'F'  frame: fences flag, die, VisualFrame fields, x and y (f32 arrays)
  'B'  bounds curve: name, title, (upper, lower) pairs
  'H'  frames held back
```

Little-endian fixed-width fields; a string is a u32 length and its bytes, an array a u64 count
and its elements. The netlist, the fences and the final placement are not recorded: the flow
still holds them when it draws. The file has no header or version: it is written and read by the
same process and deleted after, so no other build ever reads it.

A replay makes the visualizer the same calls, in the same order, with the same values (floats
are stored as their raw bytes, not printed), so the pictures are **byte-identical** to the ones
drawing during the run made. This is checked by:

- `test_viz` `replayed_snapshots_draw_the_files_drawing_directly_does`, which makes the same
  calls on the `Visualizer` directly and through a snapshot file, and compares every file;
- `test_flow` `a_run_draws_its_recorded_pictures_and_removes_the_snapshot_file`;
- by hand, before merging, against `main`: ibm01 with SimPL (155 files) and NTUPlace1 (13), and
  adaptec1 (116), all with the animation on, all byte-identical including the GIF and
  `final.png`.

## Cost

adaptec1 (SimPL, Abacus, FastDP, animation on), dev container:

| | global placement | legalize | detailed place | draw | whole run |
| --- | ---: | ---: | ---: | ---: | ---: |
| drawing in the loops (`main` before #17) | 25.6 s | 2.6 s | 6.9 s | in the stages | 78 s |
| recording, drawing at the end | 9.9 s | 1.8 s | 5.0 s | 21 s | 81 s |

The run as a whole takes as long: the drawing still happens, after `placed.pl` is written
instead of inside the solver.

## Adding a stage that draws

1. Give the stage's design struct a hook: a `FrameInfo` with the fields above, and
   `std::function<void(xs, ys, info)>`. Call it only when it is set.
2. Make the cadence an option of the stage's own (`<stage>_options.h`).
3. In `FlowMgr`, call `recordStage(dm, snapshots.get(), {...})`.
4. Do not include anything from `visualization/` in the stage.

A new kind of picture (not a placement frame) gets a `Visualizer` call, a record in the snapshot file (a
new tag) and its case in `replaySnapshots()`.

## Known gaps

- **A run that stops early draws nothing.** A fatal error exits without reaching
  `drawPictures()`, and the snapshot file is left behind with no reader for it.
- **NTUPlace1's frames are not drawn.** Its hook exists, but the flow does not install it, as
  before the refactor. Drawing them adds about one frame per cut and changes the GIF, so it is
  left as its own decision.
- **The snapshot file stores every cell, in full precision.** Fixed cells repeat in every frame.
  Dropping them would shrink the snapshot file several times.
- **The web console's liveness check counts SVG frames** (`webui/server.py`). Frames now appear
  only at the end of the run, so during placement the console has only CPU use to go on.
