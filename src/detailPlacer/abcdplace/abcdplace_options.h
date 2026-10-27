#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct AbcdplaceOptions {
    static constexpr Component component = Component::AbcdPlace;

    std::size_t reorderWindow = 4;
    std::size_t ismSetSize = 128;
    std::size_t ismIterations = 10;
    double ismMinImprovement = 1e-3;
    double ismBinRows = 4.0;
    std::size_t ismSearchRings = 8;
    std::size_t swapBatch = 256;
    double swapBinRows = 3.0;
    std::size_t seed = 1;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("window", &AbcdplaceOptions::reorderWindow),
            option("ism_set_size", &AbcdplaceOptions::ismSetSize),
            option("ism_iterations", &AbcdplaceOptions::ismIterations),
            option("ism_min_improvement", &AbcdplaceOptions::ismMinImprovement),
            option("swap_batch", &AbcdplaceOptions::swapBatch),
            option("swap_bin_rows", &AbcdplaceOptions::swapBinRows),
            option("seed", &AbcdplaceOptions::seed),
        };
    }
};

}  // namespace ktplace
