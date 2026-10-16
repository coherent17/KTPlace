#pragma once

#include "globalPlacer/simpl/simpl_fence.h"
#include "globalPlacer/simpl/simpl_graph.h"

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ktplace::simpl {

struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
};

struct RowInfo {
    double coordinate = 0.0;
    double height = 0.0;
    double sitePitch = 0.0;

    [[nodiscard]] double pitch() const noexcept {
        return sitePitch;
    }
    std::vector<Subrow> subrows;
};

using DieBox = std::array<double, 4>;

struct FrameInfo {
    std::string note;
    std::string series;
    std::size_t step = 0;
    std::size_t numSteps = 0;
    double hpwl = 0.0;
    double hpwlInitial = 0.0;
    double overflow = 0.0;
    bool mandatory = false;
};

using FrameHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                     const FrameInfo &info)>;

struct BoundsPoint {
    double upper = 0.0;
    double lower = 0.0;
};

using BoundsHook = std::function<void(std::span<const BoundsPoint> curve)>;

struct Design {
    Graph graph;
    std::vector<RowInfo> rows;
    DieBox die{};
    Fences fences;
    FrameHook onFrame;
    BoundsHook onBounds;
};

}  // namespace ktplace::simpl
