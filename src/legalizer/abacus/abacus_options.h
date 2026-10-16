// @file abacus_options.h
// Abacus legalization options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct AbacusOptions {
    static constexpr Component component = Component::Abacus;

    std::size_t maxRowDistance = 0;
    std::size_t minSubrowSites = 1;
    std::size_t frameEvery = 0;

    static void fields(auto &self, auto &&visit) {
        visit("max_row_dist", self.maxRowDistance);
        visit("min_subrow_sites", self.minSubrowSites);
        visit("frame_every", self.frameEvery);
    }
};

}  // namespace ktplace
