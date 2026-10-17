// @file ntuplace1_options.h
// NTUPlace1's options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>

namespace ktplace {

struct Ntuplace1Options {
    static constexpr Component component = Component::NtuPlace1;

    std::size_t targetLeafCells = 100;
    std::size_t maxLevels = 32;
    std::size_t maxRatioRetries = 16;
    bool verbose = false;
    std::string partitioner = "fm";

    static void fields(auto &self, auto &&visit) {
        visit("leaf_cells", self.targetLeafCells);
        visit("max_levels", self.maxLevels);
        visit("retries", self.maxRatioRetries);
        visit("verbose", self.verbose);
        visit("partitioner", self.partitioner);
    }
};

}  // namespace ktplace
