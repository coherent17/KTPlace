/**
 * @file kt_quadPlacer.h
 * @brief TBB-parallel quadratic placement solver
 */

#pragma once

#include <cstddef>
#include <string>
#include "datamodel/kt_dm.h"

namespace ktplace {

/**
 * @brief Results of a QuadraticPlacer run
 */
struct PlacerResult {
    std::size_t numMovable = 0;           ///< number of movable (solved) cells
    std::size_t numStars = 0;             ///< star nodes introduced for large nets
    std::size_t numIterations = 0;        ///< outer (WL+density) iterations used
    double finalResidual = 0.0;           ///< final CG residual (inner solve)
    double densityOverflowInitial = 0.0;  ///< initial density overflow (0..~1)
    double densityOverflowFinal = 0.0;    ///< final density overflow
    double hpwlInitial = 0.0;             ///< HPWL before placement
    double hpwlFinal = 0.0;               ///< HPWL after placement
    double buildSeconds = 0.0;            ///< matrix assembly time
    double solveSeconds = 0.0;            ///< global-placement (outer loop) time
    double hpwlSeconds = 0.0;             ///< HPWL evaluation time
};

/**
 * @brief Quadratic placement using a clique/star-hybrid net model, CSR matrix,
 *        and a Jacobi-preconditioned conjugate-gradient solve, all parallelized
 *        with oneTBB.
 *
 * Objective (from docs/QuadraticPlacement.md):
 *   Phi(x,y) = sum over nets of weighted squared deviations.
 * Meets  Q_mm * x_m = b_x  after eliminating fixed cells / star nodes.
 */
class QuadraticPlacer {
public:
    explicit QuadraticPlacer(PlacementDB &db);

    /**
     * @brief Perform global placement: an outer loop alternating a warm-started
     *        conjugate-gradient solve of the connectivity system with a
     *        density-spreading force (ePlace-style fixed-point lambda), so the
     *        movable cells spread over the die instead of collapsing onto a
     *        point. Writes the final positions back into the database.
     *
     * @param maxIter outer-iteration budget (each = 1 WL+density solve).
     * @param tol     density overflow target for early termination.
     * @param plotDir if non-empty, emit an SVG snapshot per outer iteration, an
     *                HPWL/summary curve (CSV+SVG), and an HTML gallery into
     *                this directory (see kt_plotter).
     */
    PlacerResult place(int maxIter = 200, double tol = 0.10, const std::string &plotDir = "");

private:
    PlacementDB &db;
};

}  // namespace ktplace
