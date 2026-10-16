// @file simpl_options.h
// SimPL's options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>

namespace ktplace {

struct SimplOptions {
    static constexpr Component component = Component::Simpl;

    std::size_t binsPerAxis = 0;
    double cgTol = 1e-4;
    std::size_t initMaxIters = 15;
    std::string initialNetModel = "star";
    std::size_t maxIters = 100;
    std::size_t seed = 20240607;
    bool fences = true;
    double densityLimit = 1.0;
    double alphaBase = 0.02;

    std::size_t maxLevel = 10;
    std::size_t minCellsToSplit = 4;
    double stripeAreaFraction = 0.1;
    std::size_t gapReferenceIter = 10;
    double gapRelaxedFrac = 0.25;
    double gapTightFrac = 0.05;
    std::size_t stallIters = 6;
    double gridGrowth = 1.06;
    double gridFloor = 6.0;
    std::size_t gridDelay = 5;
    std::size_t cgMaxIter = 60;
    std::size_t traceEvery = 1;
    std::size_t cgEvery = 0;

    static void fields(auto &self, auto &&visit) {
        visit("grid", self.binsPerAxis);
        visit("cg_tol", self.cgTol);
        visit("init_iters", self.initMaxIters);
        visit("init_net", self.initialNetModel);
        visit("iters", self.maxIters);
        visit("seed", self.seed);
        visit("fences", self.fences);
        visit("density", self.densityLimit);
        visit("alpha_base", self.alphaBase);
        visit("trace_every", self.traceEvery);
        visit("cg_every", self.cgEvery);
        visit("grid_growth", self.gridGrowth);
        visit("grid_floor", self.gridFloor);
        visit("grid_delay", self.gridDelay);
    }
};

}  // namespace ktplace
