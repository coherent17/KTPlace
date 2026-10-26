#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>
#include <tuple>

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

    std::size_t maxLevel = 20;
    std::size_t minCellsToSplit = 4;
    double stripeAreaFraction = 0.1;
    std::size_t gapReferenceIter = 10;
    double gapRelaxedFrac = 0.25;
    double gapTightFrac = 0.05;
    std::size_t stallIters = 6;
    std::size_t refineIters = 10;
    std::size_t refineGridFactor = 3;
    std::size_t cgMaxIter = 60;
    std::size_t traceEvery = 2;
    std::string traceSeries = "lss";
    std::size_t reportEvery = 5;
    std::size_t cgEvery = 0;
    std::size_t initCgEvery = 4;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("grid", &SimplOptions::binsPerAxis),
            option("cg_tol", &SimplOptions::cgTol),
            option("init_iters", &SimplOptions::initMaxIters),
            option("init_net", &SimplOptions::initialNetModel),
            option("iters", &SimplOptions::maxIters),
            option("seed", &SimplOptions::seed),
            option("fences", &SimplOptions::fences),
            option("density", &SimplOptions::densityLimit),
            option("alpha_base", &SimplOptions::alphaBase),
            option("trace_every", &SimplOptions::traceEvery),
            option("trace_series", &SimplOptions::traceSeries),
            option("report_every", &SimplOptions::reportEvery),
            option("cg_every", &SimplOptions::cgEvery),
            option("init_cg_every", &SimplOptions::initCgEvery),
        };
    }
};

}  // namespace ktplace
