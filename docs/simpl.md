# SimPL, and what KTPlace actually does

This note answers a recurring question: *is the quadratic placer SimPL?* Short
answer: **no** — it is a quadratic placer that borrows one idea from SimPL,
namely how to spread a collapsed solution. The wirelength solver, the density
model and the spreading mechanics are our own. This document separates the two,
and records a measured defect in the current phase schedule.

## What SimPL is

SimPL — *SimPL: An Effective Placement Algorithm* (Kim, Xu, Markov et al.,
ICCAD 2010) — is deliberately a **quadratic, force-directed** global placer.
Its selling point was being self-contained and simple compared with the
multi-stage flows of its contemporaries. Its structure is:

1. **Quadratic wirelength.** The net model is the classic one: sum of squared
   distances over pairs of connected pins, which reduces to a symmetric linear
   system in the cell coordinates.
2. **A spreading step.** Minimising squared wirelength alone drives every cell
   to the netlist's force-balance point, so the solution collapses. SimPL
   escapes it by *non-linear scaling*: it spreads the collapsed placement out
   toward a uniform-density layout, with geometric top-down partitioning doing
   the heavy lifting.
3. **A rough legalization** to turn the upper-bound solution into a concrete one
   and report a realizable wirelength.

Two things people routinely conflate with SimPL, and which are *not* SimPL:

- **Log-sum-exp (LSE) or weighted-average wirelength models.** Those belong to
  the APlace / ePlace / FastPlace / SABER lineage, which optimise
  `W(x, y) + λ D(x, y)` with a non-convex density potential (electrostatics in
  ePlace, a bell-shaped kernel in APlace) and use Nesterov's accelerated
  gradient method.
- **The `SimPL` in the topology-optimization literature** (SiMPL: Sigmoidal
  Mirror descent with Projected Linearization). Unrelated despite the name.

KTPlace is closer to the SimPL end of the spectrum than to the LSE/Nesterov
end, because it keeps quadratic wirelength. But it is not SimPL: we solve with
Jacobi-preconditioned conjugate gradient, not a bespoke force-directed scheme,
and our spreading is a bin-occupancy equi-area projection, not non-linear
scaling plus top-down partitioning.

## Why quadratic wirelength needs help

For a net `e` with pins at positions `x_i`, quadratic wirelength contributes
`Σ_{i<j} (x_i - x_j)²`, minimised when every pin sits at the net's centroid.
Minimising over *all* nets at once makes the optimum the force-balance point of
the whole netlist — typically a single point or a tight blob. So the global
minimum of the quadratic problem is a degenerate placement, and any density
handling has to be an *additional* force, not a tie-breaker.

KTPlace also re-seeds movable cells at the die centre when the input placement
is degenerate (many public Bookshelf circuits put every cell on the origin, and
the ISPD 2015 LEF/DEF suites leave standard cells `UNPLACED` entirely), so the
solve always starts from a point-like placement and always needs the spread.

## What KTPlace does

Per outer iteration, in `src/placer/kt_quadPlacer.cc`:

1. **Density grid.** The die bounding box is divided into a uniform bin grid,
   64 bins wide with the height scaled by the die aspect ratio, so bins stay
   roughly square. Movable cell area is accumulated into bins with bilinear
   (smooth) weights, in parallel.
2. **Overflow.** `densityOverflow` sums `max(occupancy - binCapacity, 0)` over
   all bins and normalises by total movable area. 0 means the die is uniformly
   covered; 1 means everything is stacked.
3. **Projection spreading** (`projectionSpread`). Cells are sorted by their
   current bin, then the accumulated area is walked across the grid and mapped
   onto a uniform target density. Each cell gets a *target point* in an
   area-balanced layout. This is the SimPL-influenced part: pull toward an
   area-balanced target, and the collapse drains outward.
   - **Gated** (default): only cells sitting in bins over
     `targetDensity * (1 + 0.15)` move; everything else keeps its current,
     wirelength-friendly position. This is the key to not destroying wirelength
     in cells that are already fine.
   - **Un-gated** (refinement phase): every cell is pulled toward its equi-area
     target, which stops the next wirelength solve from re-collapsing the
     carpet into a corner blob.
4. **Wirelength solve.** The quadratic system is assembled in CSR form and
   solved with Jacobi-preconditioned CG, warm-started from the previous
   iterate. Nets with at most 32 distinct cells become a clique; larger nets get
   star nodes to avoid the `O(k²)` blow-up.
5. **Refinement coupling** (only after the spread phase — see the defect below).
   The projection is folded into the CG right-hand side as
   `kProjMu * diag[i] * (proj[i] - x[i])`, so the spread acts as an additional
   anchored force during the solve rather than a post-hoc nudge.
6. **Capped direct displacement.** Each cell then walks toward its target by at
   most one bin width. The cap is what makes the outward drift survive the next
   solve, so over-full bins drain steadily instead of oscillating.

Parallelism: TBB `parallel_for` / `parallel_reduce` throughout — grid
accumulation, matrix assembly, both CG solves, the projection walk.

## The phase schedule was dead code (fixed)

The placer has two phases: spread first, then refine. The boundary used to be

```cpp
const std::size_t spreadInIters = std::min(kSpreadInIters, numStepsTotal);
...
if (outer >= spreadInIters) { /* refinement: un-gated pull + coupled CG */ }
```

`kSpreadInIters` was 200 and `FlowMgr` calls `place(200, 0.10, plotDir)`, so
`spreadInIters == numStepsTotal == 200`. The loop runs `outer` from 0 to 199, so
`outer >= 200` was **never true**: the refinement phase never executed. The
convergence check just below it was dead for the same reason, so the `tol`
argument was ignored and every run did all 200 iterations.

The boundary is now a fraction of the budget, so the refinement window is never
empty whatever the caller asks for:

```cpp
constexpr double kSpreadPhaseFraction = 0.5;
const std::size_t spreadInIters =
    static_cast<std::size_t>(static_cast<double>(numStepsTotal) * kSpreadPhaseFraction);
```

`tol` is live again: the run may now stop early once refinement has had half its
window and the overflow is under target.

### What the fix bought, and what it cost

Wirelength improved on every benchmark tested, by 39-43%:

| design | cells | HPWL before | HPWL after | ratio before | ratio after |
| --- | ---: | ---: | ---: | ---: | ---: |
| `mgc_des_perf_a` | 108,666 | 5.22e10 | 3.01e10 | 26.0x | 15.0x |
| `adaptec2` | 255,023 | 2.33e9 | 1.32e9 | 32.1x | 18.2x |
| `adaptec5` | 843,128 | 1.11e10 | 6.71e9 | 51.1x | 31.0x |
| `mgc_superblue16_a` | 698,367 | 5.51e11 | 3.35e11 | 15.4x | 9.3x |

**But density overflow got worse on three of the four**, which is a real
regression and not a rounding artefact:

| design | overflow before | overflow after |
| --- | ---: | ---: |
| `mgc_des_perf_a` | 0.2231 | 0.1971 |
| `adaptec2` | 0.0651 | **0.1843** |
| `adaptec5` | 0.1206 | **0.2007** |
| `mgc_superblue16_a` | 0.1794 | **0.2146** |

So the refinement phase trades density for wirelength. The un-gated equi-area
pull is meant to *hold* the carpet while the solve lowers wiring, but the coupled
CG force evidently wins often enough to let bins refill; the capped displacement
in step 6 is not enough to push them back out again. `adaptec2` is the starkest
case, going from 0.065 to 0.184.

The split ratio is not the lever. Measured at 0.3 / 0.5 / 0.7 the results sit
within noise of each other (`mgc_des_perf_a` 0.1964 / 0.1971 / 0.2002 overflow,
14.9x / 15.0x / 15.3x ratio), which says the gain comes from the refinement phase
existing at all rather than from where it starts. 0.5 is kept as a neutral
default.

The open problem is therefore the balance *within* refinement. The options, in
rough order of promise:

1. Re-apply the gated spread *after* the coupled solve, not only before it, so
   density protection has the last word each iteration.
2. Raise `kProjMu` (currently 2.0) so the anchored pull outweighs the wirelength
   gradient.
3. Accept the previous iteration's positions when a refinement step would raise
   overflow, i.e. treat overflow as a constraint rather than a report.

Until one of those lands, the numbers above are the honest state: a large
wirelength win bought with density, on a placer that still has no legalization
stage.

## Honest summary of quality

The engine reports `HPWL / seed` rather than pretending to improve on the
input. Typical ratios are 15x-51x, which is *not* competitive: it is the cost of
a post-spread, pre-legalization snapshot. Published placers land within
1.05-1.3x of a real seed because they (a) solve a density-aware problem
analytically, and (b) follow global placement with legalization and detailed
placement, which recover most of the wirelength the spread costs. KTPlace has
neither, and that — not the spreading quality — is the dominant gap.

Two further measured inefficiencies, both from the timing table:

- `write` runs at 1.00x parallelism while streaming 698k vertices.
- `load` on LEF/DEF input is effectively serial.

## Terminology

The source comment calling the spread "SimPL-style projection spreading" is
loose but not wrong: the idea of mapping accumulated area onto a uniform-density
target is in SimPL's spirit, though the mechanism here (bin-occupancy equi-area
projection with gating) is not SimPL's (non-linear scaling with geometric
top-down partitioning). "Equi-area projection spreading" would be a more
accurate name.
