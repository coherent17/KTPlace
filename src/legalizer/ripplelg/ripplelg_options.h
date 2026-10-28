#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct RipplelgOptions {
    static constexpr Component component = Component::RippleLg;

    std::size_t maxCellRows = 4;
    double windowWidth = 8.0;
    std::size_t windowRows = 2;
    std::size_t windowGrowth = 8;
    bool powerRailAlignment = true;
    bool matching = true;
    double toleratedDisplacement = 4.0;
    std::size_t matchingGroup = 128;
    bool fixedOrder = true;
    std::size_t frames = 24;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("max_rows", &RipplelgOptions::maxCellRows),
            option("window_width", &RipplelgOptions::windowWidth),
            option("window_rows", &RipplelgOptions::windowRows),
            option("window_growth", &RipplelgOptions::windowGrowth),
            option("pg_alignment", &RipplelgOptions::powerRailAlignment),
            option("matching", &RipplelgOptions::matching),
            option("delta0", &RipplelgOptions::toleratedDisplacement),
            option("matching_group", &RipplelgOptions::matchingGroup),
            option("fixed_order", &RipplelgOptions::fixedOrder),
            option("frames", &RipplelgOptions::frames),
        };
    }
};

}  // namespace ktplace
