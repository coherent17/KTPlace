#pragma once

#include "geometry/kt_bbox.h"
#include "util/kt_parallel.h"

#include <cstddef>
#include <ranges>

namespace ktplace {

template <typename NetPins, typename PinPosition>
[[nodiscard]] double getNetHpwl(std::size_t numNets, NetPins netPins, PinPosition pinPosition) {
    return getOrderedSum(numNets, [&](std::size_t n) {
        decltype(auto) pins = netPins(n);
        if (std::ranges::size(pins) < 2) {
            return 0.0;
        }
        BBox box = BBox::getEmpty();
        for (const auto &pin : pins) {
            const auto [px, py] = pinPosition(pin);
            box.extend(px, py);
        }
        return box.getHalfPerimeter();
    });
}

}  // namespace ktplace
