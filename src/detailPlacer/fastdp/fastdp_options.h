// @file fastdp_options.h
// FastDP detailed placement options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct FastdpOptions {
    static constexpr Component kComponent = Component::FastDp;

    std::size_t localReorderWindow = 0;
    bool noMedian = false;

    std::size_t globalSwapPasses = 4;
    std::size_t verticalSwapPasses = 2;
    std::size_t localReorderPasses = 2;
    std::size_t clusterPasses = 2;
    double minImprovement = 1e-4;

    static void fields(auto &self, auto &&visit) {
        visit("window", self.localReorderWindow);
        visit("no_median", self.noMedian);
    }
};

}  // namespace ktplace
