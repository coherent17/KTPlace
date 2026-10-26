#include "datamodel/kt_routingDemand.h"

#include "datamodel/kt_graph.h"

#include <algorithm>
#include <cmath>

namespace ktplace {

namespace {

constexpr double degenerateFraction = 1e-3;
constexpr double hotFactor = 2.0;
constexpr float faintDemand = 0.03F;

}  // namespace

NetlistPins NetlistPins::of(const Graph &graph) {
    NetlistPins pins;
    pins.netPinStart.reserve(graph.getNumNets() + 1);
    pins.netPinStart.push_back(0);
    for (std::size_t net = 0; net < graph.getNumNets(); ++net) {
        for (const std::size_t pinId : graph.getNetPins(net)) {
            const Pin &pin = graph.getPin(pinId);
            pins.pinCell.push_back(static_cast<std::uint32_t>(pin.cellId));
            pins.pinDx.push_back(static_cast<float>(pin.offsetX));
            pins.pinDy.push_back(static_cast<float>(pin.offsetY));
        }
        pins.netPinStart.push_back(static_cast<std::uint32_t>(pins.pinCell.size()));
    }
    return pins;
}

RoutingDemand getRoutingDemand(std::span<const std::uint32_t> netPinStart,
                               std::span<const std::uint32_t> pinCell, std::span<const float> pinDx,
                               std::span<const float> pinDy, std::span<const float> x,
                               std::span<const float> y, const BBox &die, std::size_t binsAcross) {
    RoutingDemand map;
    const double spanX = std::max(die.upperRight.x - die.lowerLeft.x, 1e-9);
    const double spanY = std::max(die.upperRight.y - die.lowerLeft.y, 1e-9);
    const double bin =
        std::max(spanX, spanY) / static_cast<double>(std::max<std::size_t>(binsAcross, 1));
    map.nx = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(spanX / bin)));
    map.ny = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(spanY / bin)));
    map.x0 = die.lowerLeft.x;
    map.y0 = die.lowerLeft.y;
    map.binW = bin;
    map.binH = bin;
    map.demand.assign(map.nx * map.ny, 0.0F);
    const double minExtent = degenerateFraction * bin;

    const std::size_t netCount = netPinStart.empty() ? 0 : netPinStart.size() - 1;
    for (std::size_t net = 0; net < netCount; ++net) {
        const std::uint32_t first = netPinStart[net];
        const std::uint32_t last = netPinStart[net + 1];
        if (last - first < 2) {
            continue;
        }
        BBox box = BBox::getEmpty();
        for (std::uint32_t p = first; p < last; ++p) {
            const std::uint32_t cell = pinCell[p];
            box.extend(x[cell] + pinDx[p], y[cell] + pinDy[p]);
        }
        double xl = box.lowerLeft.x;
        double xh = box.upperRight.x;
        double yl = box.lowerLeft.y;
        double yh = box.upperRight.y;
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

std::array<std::uint8_t, 4> getDemandColour(float scaled) {
    const float v = std::clamp(scaled, 0.0F, 1.0F);
    struct Stop {
        float at;
        float r, g, b;
    };
    constexpr std::array<Stop, 5> stops{{{0.0F, 0x1f, 0x4e, 0xd8},
                                         {0.35F, 0x1f, 0xb5, 0x8f},
                                         {0.6F, 0xe6, 0xd2, 0x2e},
                                         {0.8F, 0xf0, 0x8a, 0x24},
                                         {1.0F, 0xe5, 0x2b, 0x2b}}};
    std::size_t k = 1;
    while (k + 1 < stops.size() && v > stops[k].at) {
        ++k;
    }
    const Stop &a = stops[k - 1];
    const Stop &b = stops[k];
    const float t = std::clamp((v - a.at) / (b.at - a.at), 0.0F, 1.0F);
    const auto mix = [t](float p, float q) {
        return static_cast<std::uint8_t>(std::lround(p + (q - p) * t));
    };
    const auto alpha = static_cast<std::uint8_t>(
        v < faintDemand ? 0 : std::lround(255.0F * 0.78F * std::pow(v, 0.6F)));
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), alpha};
}

}  // namespace ktplace
