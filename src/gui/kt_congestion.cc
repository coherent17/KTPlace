#include "gui/kt_congestion.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ktplace::gui {

namespace {

constexpr double degenerateFraction = 1e-3;
constexpr double hotFactor = 2.0;

}  // namespace

DemandMap estimateDemand(const LayoutGeometry &geometry, const std::vector<float> &x,
                         const std::vector<float> &y, std::size_t binsAcross) {
    DemandMap map;
    const auto &die = geometry.die;
    const double spanX = std::max(die[2] - die[0], 1e-9);
    const double spanY = std::max(die[3] - die[1], 1e-9);
    const double bin =
        std::max(spanX, spanY) / static_cast<double>(std::max<std::size_t>(binsAcross, 1));
    map.nx = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(spanX / bin)));
    map.ny = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(spanY / bin)));
    map.x0 = die[0];
    map.y0 = die[1];
    map.binW = bin;
    map.binH = bin;
    map.demand.assign(map.nx * map.ny, 0.0F);
    const double minExtent = degenerateFraction * bin;

    const std::size_t netCount = geometry.netPinStart.empty() ? 0 : geometry.netPinStart.size() - 1;
    for (std::size_t net = 0; net < netCount; ++net) {
        const std::uint32_t first = geometry.netPinStart[net];
        const std::uint32_t last = geometry.netPinStart[net + 1];
        if (last - first < 2) {
            continue;
        }
        double xl = std::numeric_limits<double>::max();
        double xh = std::numeric_limits<double>::lowest();
        double yl = xl;
        double yh = xh;
        for (std::uint32_t p = first; p < last; ++p) {
            const std::uint32_t cell = geometry.pinCell[p];
            const double px = x[cell] + geometry.pinDx[p];
            const double py = y[cell] + geometry.pinDy[p];
            xl = std::min(xl, px);
            xh = std::max(xh, px);
            yl = std::min(yl, py);
            yh = std::max(yh, py);
        }
        if (xh - xl < minExtent) {
            const double c = (xl + xh) / 2.0;
            xl = c - minExtent / 2.0;
            xh = c + minExtent / 2.0;
        }
        if (yh - yl < minExtent) {
            const double c = (yl + yh) / 2.0;
            yl = c - minExtent / 2.0;
            yh = c + minExtent / 2.0;
        }
        const double density = 1.0 / ((xh - xl) * (yh - yl));
        const auto binOf = [](double v, double origin, double size, std::size_t count) {
            const auto i = static_cast<long>(std::floor((v - origin) / size));
            return static_cast<std::size_t>(std::clamp<long>(i, 0, static_cast<long>(count) - 1));
        };
        const std::size_t ix0 = binOf(xl, map.x0, map.binW, map.nx);
        const std::size_t ix1 = binOf(xh, map.x0, map.binW, map.nx);
        const std::size_t iy0 = binOf(yl, map.y0, map.binH, map.ny);
        const std::size_t iy1 = binOf(yh, map.y0, map.binH, map.ny);
        for (std::size_t iy = iy0; iy <= iy1; ++iy) {
            const double by0 = map.y0 + static_cast<double>(iy) * map.binH;
            const double oy = std::min(yh, by0 + map.binH) - std::max(yl, by0);
            if (oy <= 0.0) {
                continue;
            }
            for (std::size_t ix = ix0; ix <= ix1; ++ix) {
                const double bx0 = map.x0 + static_cast<double>(ix) * map.binW;
                const double ox = std::min(xh, bx0 + map.binW) - std::max(xl, bx0);
                if (ox > 0.0) {
                    map.demand[iy * map.nx + ix] += static_cast<float>(density * ox * oy);
                }
            }
        }
        ++map.nets;
    }

    double sum = 0.0;
    for (std::size_t i = 0; i < map.demand.size(); ++i) {
        sum += map.demand[i];
        if (map.demand[i] > map.peak) {
            map.peak = map.demand[i];
            map.peakBin = i;
        }
    }
    map.mean = static_cast<float>(sum / static_cast<double>(map.demand.size()));
    std::vector<float> sorted = map.demand;
    const auto rank = sorted.begin() + static_cast<std::ptrdiff_t>((sorted.size() - 1) * 99 / 100);
    std::nth_element(sorted.begin(), rank, sorted.end());
    map.p99 = *rank;
    const auto hot = std::count_if(map.demand.begin(), map.demand.end(), [&map](float v) {
        return v > hotFactor * map.mean;
    });
    map.hotShare = static_cast<double>(hot) / static_cast<double>(map.demand.size());
    return map;
}

}  // namespace ktplace::gui
