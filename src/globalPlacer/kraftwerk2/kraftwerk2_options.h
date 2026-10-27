#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct Kraftwerk2Options {
    static constexpr Component component = Component::Kraftwerk2;

    std::size_t binsPerAxis = 0;
    std::size_t initIters = 5;
    std::size_t maxIters = 1000;
    double targetOverlap = 0.2;
    double targetMovement = 0.0;
    double targetDensity = 1.0;
    double largeModuleArea = 50.0;
    double cgTol = 1e-5;
    std::size_t cgMaxIter = 150;
    std::size_t traceEvery = 2;
    std::size_t reportEvery = 5;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("grid", &Kraftwerk2Options::binsPerAxis),
            option("init_iters", &Kraftwerk2Options::initIters),
            option("iters", &Kraftwerk2Options::maxIters),
            option("overlap", &Kraftwerk2Options::targetOverlap),
            option("movement", &Kraftwerk2Options::targetMovement),
            option("density", &Kraftwerk2Options::targetDensity),
            option("large_area", &Kraftwerk2Options::largeModuleArea),
            option("cg_tol", &Kraftwerk2Options::cgTol),
            option("cg_iters", &Kraftwerk2Options::cgMaxIter),
            option("trace_every", &Kraftwerk2Options::traceEvery),
            option("report_every", &Kraftwerk2Options::reportEvery),
        };
    }
};

}  // namespace ktplace
