// @file ntuplace1_options.h
// NTUPlace1's options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct Ntuplace1Options {
    static constexpr Component component = Component::NtuPlace1;

    std::size_t targetLeafCells = 100;
    std::size_t maxLevels = 32;
    std::size_t maxRatioRetries = 4;
    bool verbose = false;

    static void fields(auto &self, auto &&visit) {
        visit("leaf_cells", self.targetLeafCells);
        visit("max_levels", self.maxLevels);
        visit("retries", self.maxRatioRetries);
        visit("verbose", self.verbose);
    }
};

}  // namespace ktplace
