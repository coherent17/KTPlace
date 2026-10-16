#pragma once

#include "globalPlacer/simpl/simpl_design.h"
#include "globalPlacer/simpl/simpl_options.h"

#include <cstddef>
#include <memory>

namespace ktplace::simpl {

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
    explicit simplPlacer(Design &design);
    ~simplPlacer();

    simplPlacer(const simplPlacer &) = delete;
    simplPlacer &operator=(const simplPlacer &) = delete;
    simplPlacer(simplPlacer &&) noexcept;
    simplPlacer &operator=(simplPlacer &&) noexcept;

    SimplResult place(const SimplOptions &options);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::simpl
