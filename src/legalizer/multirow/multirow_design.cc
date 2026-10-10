#include "legalizer/multirow/multirow_design.h"

#include <algorithm>
#include <limits>

namespace ktplace::multirow {

void Region::seal() {
    if (rects.empty()) {
        minX = minY = maxX = maxY = 0.0;
        return;
    }
    minX = minY = std::numeric_limits<double>::max();
    maxX = maxY = std::numeric_limits<double>::lowest();
    for (const Rect &r : rects) {
        minX = std::min(minX, r.xlo);
        minY = std::min(minY, r.ylo);
        maxX = std::max(maxX, r.xhi);
        maxY = std::max(maxY, r.yhi);
    }
}

std::size_t Fences::countViolations(const std::vector<double> &positions,
                                    const std::vector<int> &regionIds) const {
    std::size_t bad = 0;
    const std::size_t n = std::min(regionIds.size(), positions.size() / 2);
    for (std::size_t i = 0; i < n; ++i) {
        const int id = regionIds[i];
        if (id < 0 || static_cast<std::size_t>(id) >= regions.size()) {
            continue;
        }
        if (!regions[static_cast<std::size_t>(id)].contains(positions[2 * i],
                                                            positions[2 * i + 1])) {
            ++bad;
        }
    }
    return bad;
}

double netlistHPWL(const std::vector<Cell> &, const std::vector<Net> &nets,
                   const std::vector<Pin> &pins, const std::vector<double> &xs,
                   const std::vector<double> &ys) {
    double total = 0.0;
    for (const Net &net : nets) {
        if (net.pins.size() < 2) {
            continue;
        }
        double xlo = std::numeric_limits<double>::max();
        double xhi = std::numeric_limits<double>::lowest();
        double ylo = std::numeric_limits<double>::max();
        double yhi = std::numeric_limits<double>::lowest();
        for (const std::size_t pinId : net.pins) {
            const Pin &pin = pins.at(pinId);
            const double px = xs.at(pin.cellId) + pin.offsetX;
            const double py = ys.at(pin.cellId) + pin.offsetY;
            xlo = std::min(xlo, px);
            xhi = std::max(xhi, px);
            ylo = std::min(ylo, py);
            yhi = std::max(yhi, py);
        }
        total += (xhi - xlo) + (yhi - ylo);
    }
    return total;
}

}  // namespace ktplace::multirow
