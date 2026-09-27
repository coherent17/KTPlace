/**
 * @file kt_simpl.h
 * @brief SimPL global placement (Kim, Lee & Markov, CACM 56(6):105-113, 2013)
 *
 * Self-contained force-directed global placement. Unlike the electrostatic
 * placers (ePlace/RePlAce) there is no potential solve here; the spreading
 * information comes from a *legalized* placement, which is what makes the
 * method both fast and its convergence provable.
 *
 * The flow keeps two placements and lets them meet:
 *
 *   lower bound  the solution of a sparse linear system, which may overlap
 *   upper bound  a roughly legal placement produced by look-ahead legalization
 *
 * The upper bound is computed first each iteration and then used two ways: its
 * locations become *fixed anchors* wired to their cells by artificial
 * two-pin pseudonets, and it becomes the linearization point for the net model.
 * The lower bound is then re-solved with those anchors in the system, which
 * drags it toward feasibility. Because the pseudonet weight grows with the
 * iteration number, the emphasis moves steadily from interconnect reduction to
 * constraint satisfaction, and convergence is guaranteed rather than hoped for.
 *
 * Three pieces carry the method, and each is implemented here as described:
 *
 *  - BOUND2BOUND (B2B) net model. A p-pin net is decomposed *for the current
 *    placement* into edges between the extreme (min and max) pins and every
 *    other pin, each of weight w / ((k-1) * |distance|). For that placement the
 *    quadratic objective then equals the net's bounding-box length exactly, so
 *    the solve is a faithful first-order model of HPWL. The catch is that the
 *    model is placement-dependent, so the graph is rebuilt every iteration --
 *    the "B2B Graph Update" box in Figure 2 of the paper.
 *
 *  - LOOK-AHEAD LEGALIZATION (Algorithm 1). Overfull grid bins are clustered by
 *    BFS; each cluster is grown to a minimal containing rectangle of legal
 *    density; that rectangle is recursively split by a cell-area cutline and a
 *    whitespace cutline, and cells are redistributed by per-stripe linear
 *    scaling. Scaling factors differ per stripe, which is where the
 *    "nonlinearity" comes from, and that is what removes overlap while
 *    preserving relative placement.
 *
 *  - PSEUDONETS. Fixed zero-area anchors at the upper-bound locations, with
 *    weight alpha / distance and alpha = 0.01 * (1 + iteration). Because a fixed
 *    endpoint contributes only to the diagonal, these also improve the
 *    conditioning of the Jacobi-preconditioned CG solve.
 *
 * References: M.-C. Kim, D.-J. Lee, I. L. Markov, "SimPL: an algorithm for
 * placing VLSI circuits", CACM 56(6), 105-113, 2013 (journal version: IEEE
 * TCAD 31(1):50-60, 2012). The B2B model is from P. Spindler et al.,
 * "Kraftwerk2", IEEE TCAD 27(8), 2008.
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "datamodel/kt_dm.h"

namespace ktplace {

/// Tunables. Defaults follow the paper: target density g = 1.0, five to seven
/// initial-placement iterations, 33-45 global-placement iterations on ISPD 2005.
struct SimplParams {
    // --- initial placement (ignores areas and overlaps entirely) ------------
    /// Alternate B2B rebuild and CG solve until HPWL stops improving. The paper
    /// reports 5-7 iterations being sufficient.
    std::size_t initMaxIters = 10;

    // --- look-ahead legalization --------------------------------------------
    /// Maximum allowed bin density, g. The paper's ISPD 2005 runs use 1.0.
    double densityLimit = 1.0;
    /// Bins per axis for the density grid. 0 selects automatically.
    std::size_t binsX = 0;
    std::size_t binsY = 0;
    /// Number of look-ahead legalization passes applied to the lower bound each
    /// outer iteration. One pass is not enough once the top-down partitioning is
    /// given the whole die: it spreads into the space and leaves holes mid-die.
    /// The isolated measurement (KTPLACE_SIMPL_LAL_ONLY) reached 0.026 overflow
    /// in two passes on adaptec1, so the machinery converges -- the loop just
    /// never gave it the rounds. Passes stop early once the overflow stops
    /// improving.
    std::size_t lalPasses = 4;
    /// Stop the internal passes once the overflow improves by less than this
    /// relative amount.
    double lalMinGain = 0.01;
    /// Recursion cut-off from Algorithm 1 line 8: blocks at this depth stop
    /// being split.
    std::size_t maxLevel = 10;
    /// Algorithm 1 line 8 "Area(B) is small enough". The paper does not give a
    /// value; blocks holding no more than this many cells are not split. A
    /// single cell cannot overlap anything, so 1 is always safe.
    std::size_t minCellsToSplit = 4;
    /// A stripe is subdivided further while its available area exceeds this
    /// fraction of the region's available area (1/10 in the paper).
    double stripeAreaFraction = 0.1;
    /// A cluster whose own rectangle already holds at least this fraction of the
    /// movable cell area means the placement is globally collapsed, not locally
    /// overfull, so the top-down partitioning is given the whole usable die instead
    /// of the minimal legal-density rectangle. Without this the legalizer confines
    /// a collapsed placement to the ~54% of the die its cells strictly need and
    /// never uses the rest -- measured as a fully empty left quarter and top third,
    /// at a large wirelength cost because the I/O pads ring the die.
    double globalClusterFrac = 0.5;

    // --- pseudonets ---------------------------------------------------------
    /// alpha = alphaBase * (1 + iteration number).
    double alphaBase = 0.01;

    /// The pseudonet weight law. The paper's Figure 6 labels a pseudonet
    /// "weight = alpha/Length", and AMF-Placer (ICCAD 2021) states it
    /// independently: "the weight of a pseudo net is calculated by dividing a
    /// global factor alpha by the movement distance of the corresponding
    /// instance in last optimization". That makes the quadratic energy
    /// w*(x-a)^2 ~ alpha*|x-a|, a constant-force L1 penalty whose restoring force
    /// does NOT grow with distance, which is what gives the linear solver
    /// "freedom" to keep optimising wirelength for a cell the legalizer moved
    /// a long way. ConstantStiffness instead makes it an L2 penalty, whose force
    /// grows linearly with distance, so distant cells get pinned to the anchor
    /// and the lower bound is dragged onto the upper bound.
    enum class PseudonetLaw {
        InverseLength,      ///< w = alpha / max(distance, floor) -- the paper's reading
        ConstantStiffness,  ///< w = alpha
    };
    PseudonetLaw pseudonetLaw = PseudonetLaw::InverseLength;

    // --- convergence --------------------------------------------------------
    /// Relative to the gap at `gapReferenceIter`: stop once the gap falls below
    /// 25% of it and the upper bound has stopped improving, or below 10% of it.
    double gapRelaxedFrac = 0.25;
    double gapTightFrac = 0.10;
    std::size_t gapReferenceIter = 10;
    std::size_t patience = 5;  // upper-bound non-improving iterations tolerated

    // --- solver -------------------------------------------------------------
    // CG budget. The paper's claim is that, with preconditioning, the iteration
    // count grows no faster than log n -- about 18 for 2e5 cells -- and that one
    // placement iteration costs O(n log^2 n). A 200-iteration cap is not part of
    // the method.
    std::size_t cgMaxIter = 60;
    // Relative residual. This is deliberately loose. The B2B objective is a
    // first-order linearisation of HPWL that is exact only at the point it was
    // built, so the solve is there to supply a descent direction, not an exact
    // minimiser; and B2B's 1/length weights span orders of magnitude, which
    // leaves Jacobi CG stalling near 1e-3 no matter how long it runs. Measured
    // with a 1e-6 target: every solve used all 200 iterations and still ended
    // between 3e-4 and 1.4e-2, i.e. the budget bought nothing.
    double cgTol = 1e-3;

    // --- run ----------------------------------------------------------------
    std::size_t maxIters = 100;
    /// Write an SVG frame every N global-placement iterations (0 = only the
    /// final frame). Requires a snapshot directory.
    std::size_t traceEvery = 0;
    /// Fixed seed for the uniform initial placement, so runs are reproducible.
    std::uint64_t seed = 20240607;

    /// Where the starting placement comes from.
    enum class StartPlacement {
        /// Use the design's own placement when it looks like a real one, and fall
        /// back to the paper's uniform seed otherwise. Not the default: measured
        /// on dma, which does ship a placement at 4.33e8, adopting it and then
        /// running the loop ends at 4.13e9, so the loop still destroys a good
        /// input. Enable it once the loop stops regressing real placements.
        Auto,
        /// The paper's Figure 2: a uniformly distributed placement, then the
        /// area-blind quadratic initial placement of Section 4.1. Correct when
        /// the design ships no usable placement.
        Uniform,
        /// Trust the design's own placement unconditionally.
        Input,
    };
    /// Uniform, the paper's behaviour, is the default: the ISPD 2005 benchmarks
    /// ship every movable cell at the origin, so there is nothing there to adopt,
    /// and adopting a real placement currently makes the result worse.
    StartPlacement start = StartPlacement::Uniform;

    /// Minimum fraction of movable cells that must lie inside the die for the
    /// input placement to count as usable under StartPlacement::Auto.
    double minInputInsideFrac = 0.9;
};

/// Outcome of a run.
struct SimplResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;

    /// Iterations actually run, initial placement included.
    std::size_t initIters = 0;
    std::size_t globalIters = 0;

    std::size_t binsX = 0;
    std::size_t binsY = 0;

    /// HPWL of the placement the design shipped with, when one was adopted.
    double hpwlInput = 0.0;
    /// Whether the input placement was adopted instead of a uniform seed.
    bool usedInputPlacement = false;
    /// HPWL of the uniform seed, of the last lower bound, and of the returned
    /// (last upper-bound) placement. The paper's result is the upper bound.
    double hpwlSeed = 0.0;
    double hpwlLower = 0.0;
    double hpwlFinal = 0.0;
    /// Final gap, hpwlFinal - hpwlLower.
    double gap = 0.0;

    /// Scaled overflow per bin (Figure 7) of the final lower bound and of the
    /// returned placement.
    double overflowLower = 0.0;
    double overflowFinal = 0.0;

    double spreadSeconds = 0.0;
    double buildSeconds = 0.0;
    double solveSeconds = 0.0;
};

class SimplePlacer {
public:
    explicit SimplePlacer(PlacementDB &db);
    ~SimplePlacer();

    SimplePlacer(const SimplePlacer &) = delete;
    SimplePlacer &operator=(const SimplePlacer &) = delete;
    SimplePlacer(SimplePlacer &&) noexcept;
    SimplePlacer &operator=(SimplePlacer &&) noexcept;

    /**
     * @brief Run global placement.
     *
     * @param params      tunables
     * @param plotDir     directory for the progress curve, or empty
     * @param snapshotDir directory for SVG frames, or empty
     */
    SimplResult place(const SimplParams &params = {}, const std::string &plotDir = "",
                      const std::string &snapshotDir = "");

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
