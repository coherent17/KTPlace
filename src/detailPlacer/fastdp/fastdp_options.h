// @file fastdp_options.h
// FastDP detailed placement options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct FastdpOptions {
    static constexpr Component component = Component::FastDp;

    std::size_t reorderWindow = 3;
    double penaltyWeight1 = 0.1;
    double penaltyWeight2 = 1.0;
    std::size_t maxIterations = 20;
    double minImprovement = 1e-3;

    bool globalSwap = true;
    bool verticalSwap = true;
    bool localReorder = true;
    bool clustering = true;
    std::size_t swapCandidates = 64;
    std::size_t verticalCandidates = 3;
    std::size_t clusterBounds = 16;

    static void fields(auto &self, auto &&visit) {
        visit("window", self.reorderWindow);
        visit("wt1", self.penaltyWeight1);
        visit("wt2", self.penaltyWeight2);
        visit("max_iterations", self.maxIterations);
        visit("min_improvement", self.minImprovement);
    }
};

}  // namespace ktplace
