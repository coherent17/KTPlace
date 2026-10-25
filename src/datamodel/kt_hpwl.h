#pragma once

#include "util/kt_parallel.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <ranges>

namespace ktplace {

template <typename NetPins, typename PinPosition>
[[nodiscard]] double netHpwl(std::size_t numNets, NetPins netPins, PinPosition pinPosition) {
    return orderedSum(numNets, [&](std::size_t n) {
        decltype(auto) pins = netPins(n);
        if (std::ranges::size(pins) < 2) {
            return 0.0;
        }
        double xlo = std::numeric_limits<double>::max();
        double xhi = std::numeric_limits<double>::lowest();
        double ylo = std::numeric_limits<double>::max();
        double yhi = std::numeric_limits<double>::lowest();
        for (const auto &pin : pins) {
            const auto [px, py] = pinPosition(pin);
            xlo = std::min(xlo, px);
            xhi = std::max(xhi, px);
            ylo = std::min(ylo, py);
            yhi = std::max(yhi, py);
        }
        return (xhi - xlo) + (yhi - ylo);
    });
}

}  // namespace ktplace
