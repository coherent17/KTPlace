// @file simpl_placer.h
// SimPL global placement (Kim, Lee & Markov, CACM 56(6):105-113, 2013)

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "placer/simpl/simpl_design.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace ktplace::simpl {

struct SimplParams {
    std::size_t initMaxIters = 15;
    double densityLimit = 1.0;
    std::size_t binsPerAxis = 0;
    std::size_t maxLevel = 10;
    std::size_t minCellsToSplit = 4;
    double stripeAreaFraction = 0.1;
    double alphaBase = 0.01;
    std::size_t gapReferenceIter = 10;
    double gapRelaxedFrac = 0.25;
    double gapTightFrac = 0.10;
    std::size_t cgMaxIter = 60;
    double cgTol = 1e-4;
    std::size_t maxIters = 100;
    std::size_t traceEvery = 1;
    std::size_t cgEvery = 0;
    bool densityMaps = true;
    std::uint64_t seed = 20240607;
};

struct SimplResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;

    std::size_t initIters = 0;
    std::size_t globalIters = 0;

    std::size_t binsX = 0;
    std::size_t binsY = 0;

    double hpwlSeed = 0.0;
    double hpwlInit = 0.0;
    double hpwlLower = 0.0;
    double hpwlFinal = 0.0;
    double gap = 0.0;

    double overflowLower = 0.0;
    double overflowFinal = 0.0;

    std::size_t fenceRegions = 0;
    std::size_t fenceClamps = 0;
    std::size_t fencePushes = 0;
    std::size_t fenceViolations = 0;

    double spreadSeconds = 0.0;
    double buildSeconds = 0.0;
    double solveSeconds = 0.0;
};

class simplPlacer {
public:
    explicit simplPlacer(const Design &design);
    ~simplPlacer();

    simplPlacer(const simplPlacer &) = delete;
    simplPlacer &operator=(const simplPlacer &) = delete;
    simplPlacer(simplPlacer &&) noexcept;
    simplPlacer &operator=(simplPlacer &&) noexcept;

    SimplResult place(const SimplParams &params, bool useFences);

    [[nodiscard]] const simplSolution &solution() const noexcept {
        return answer;
    }

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
    simplSolution answer;
};

}  // namespace ktplace::simpl
