// @file ntuplace1_options.h
// NTUPlace1's options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct Ntuplace1Options {
    static constexpr Component component = Component::NtuPlace1;

    std::size_t targetLeafCells = 100;
    std::size_t maxLevels = 32;
    std::size_t maxRatioRetries = 4;
    bool verbose = false;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("leaf_cells", &Ntuplace1Options::targetLeafCells),
            option("max_levels", &Ntuplace1Options::maxLevels),
            option("retries", &Ntuplace1Options::maxRatioRetries),
            option("verbose", &Ntuplace1Options::verbose),
        };
    }
};

}  // namespace ktplace
