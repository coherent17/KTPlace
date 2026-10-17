#pragma once

#include "gui/kt_layout.h"

#include <cstddef>
#include <vector>

namespace ktplace::gui {

struct DemandMap {
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

[[nodiscard]] DemandMap estimateDemand(const LayoutGeometry &geometry, const std::vector<float> &x,
                                       const std::vector<float> &y, std::size_t binsAcross);

}  // namespace ktplace::gui
