// @file multirow_options.h
// Multi-row legalization options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct MultiRowOptions {
    static constexpr Component component = Component::MultiRow;

    std::size_t macroRows = 3;
    std::size_t frames = 24;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("macro_rows", &MultiRowOptions::macroRows),
            option("frames", &MultiRowOptions::frames),
        };
    }
};

}  // namespace ktplace
