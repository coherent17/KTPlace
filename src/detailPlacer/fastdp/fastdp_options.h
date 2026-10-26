// @file fastdp_options.h
// FastDP detailed placement options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct FastdpOptions {
    static constexpr Component component = Component::FastDp;

    std::size_t reorderWindow = 3;
    double penaltyWeight1 = 0.1;
    double penaltyWeight2 = 1.0;
    std::size_t maxIterations = 20;
    double minImprovement = 2e-4;

    bool globalSwap = true;
    bool verticalSwap = true;
    bool localReorder = true;
    bool clustering = true;
    std::size_t swapCandidates = 64;
    std::size_t verticalCandidates = 3;
    std::size_t clusterBounds = 16;
    std::size_t verifiedCandidates = 3;
    std::size_t verifyReach = 8;
    std::size_t weakPassLimit = 2;
    std::size_t parallelBatch = 4096;
    std::size_t refreshCandidates = 8;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("window", &FastdpOptions::reorderWindow),
            option("wt1", &FastdpOptions::penaltyWeight1),
            option("wt2", &FastdpOptions::penaltyWeight2),
            option("max_iterations", &FastdpOptions::maxIterations),
            option("min_improvement", &FastdpOptions::minImprovement),
            option("parallel_batch", &FastdpOptions::parallelBatch),
            option("refresh_candidates", &FastdpOptions::refreshCandidates),
        };
    }
};

}  // namespace ktplace
