#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace ktplace {

class Graph;

struct NetlistPins {
    std::vector<std::uint32_t> netPinStart;
    std::vector<std::uint32_t> pinCell;
    std::vector<float> pinDx;
    std::vector<float> pinDy;

    [[nodiscard]] static NetlistPins of(const Graph &graph);
};

struct RoutingDemand {
    std::size_t nx = 0;
    std::size_t ny = 0;
    double x0 = 0.0;
    double y0 = 0.0;
    double binW = 1.0;
    double binH = 1.0;
    std::vector<float> demand;
    std::size_t nets = 0;
    float peak = 0.0F;
    std::size_t peakBin = 0;
    float mean = 0.0F;
    float p99 = 0.0F;
    double hotShare = 0.0;

    [[nodiscard]] float at(std::size_t ix, std::size_t iy) const {
        return demand[iy * nx + ix];
    }
};

[[nodiscard]] RoutingDemand routingDemand(std::span<const std::uint32_t> netPinStart,
                                          std::span<const std::uint32_t> pinCell,
                                          std::span<const float> pinDx,
                                          std::span<const float> pinDy, std::span<const float> x,
                                          std::span<const float> y,
                                          const std::array<double, 4> &die, std::size_t binsAcross);

[[nodiscard]] std::array<std::uint8_t, 4> demandColour(float scaled);

}  // namespace ktplace
