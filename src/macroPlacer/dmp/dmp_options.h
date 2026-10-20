// @file dmp_options.h
// Detailed macro placement options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

namespace ktplace {

struct DmpOptions {
    static constexpr Component component = Component::Dmp;

    std::size_t passes = 4;
    std::size_t gridSteps = 16;
    bool flip = true;
    bool shift = true;
    bool snap = false;
    bool swap = false;
    double shiftGain = 0.0;

    static void fields(auto &self, auto &&visit) {
        visit("passes", self.passes);
        visit("grid_steps", self.gridSteps);
        visit("flip", self.flip);
        visit("shift", self.shift);
        visit("snap", self.snap);
        visit("swap", self.swap);
        visit("shift_gain", self.shiftGain);
    }
};

}  // namespace ktplace
