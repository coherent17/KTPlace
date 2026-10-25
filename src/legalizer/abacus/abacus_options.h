// @file abacus_options.h
// Abacus legalization options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct AbacusOptions {
    static constexpr Component component = Component::Abacus;

    std::size_t maxRowDistance = 0;
    std::size_t minSubrowSites = 1;
    std::size_t frames = 24;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("max_row_dist", &AbacusOptions::maxRowDistance),
            option("min_subrow_sites", &AbacusOptions::minSubrowSites),
            option("frames", &AbacusOptions::frames),
        };
    }
};

}  // namespace ktplace
