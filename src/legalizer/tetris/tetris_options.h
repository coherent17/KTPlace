#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct TetrisOptions {
    static constexpr Component component = Component::Tetris;

    double widthFactor = 0.5;
    double leftFactor = 1.0;
    double yWeight = 1.0;
    std::size_t frames = 24;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("width_factor", &TetrisOptions::widthFactor),
            option("left_factor", &TetrisOptions::leftFactor),
            option("y_weight", &TetrisOptions::yWeight),
            option("frames", &TetrisOptions::frames),
        };
    }
};

}  // namespace ktplace
