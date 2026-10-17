// @file multirow_options.h
// Multi-row legalization options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct MultiRowOptions {
    static constexpr Component component = Component::MultiRow;

    std::size_t macroRows = 3;
    std::size_t frames = 40;

    static void fields(auto &self, auto &&visit) {
        visit("macro_rows", self.macroRows);
        visit("frames", self.frames);
    }
};

}  // namespace ktplace
