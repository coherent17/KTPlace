#include "detailPlacer/fastdp/fastdp_design.h"

#include <algorithm>
#include <limits>

namespace ktplace::fastdp {

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

}  // namespace ktplace::fastdp
