// @file ntuplace1_design.h
// The design as NTUPlace1 sees it.

#pragma once

#include "constraint/kt_constraintMgr.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ktplace::ntuplace1 {

struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    int region = constraintMgr::kNoRegion;
};

struct Pin {
    std::size_t cellId = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

struct Net {
    std::vector<std::size_t> pins;
};

struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
};

struct RowInfo {
    double coordinate = 0.0;
    double height = 0.0;
    std::vector<Subrow> subrows;
};

using DieBox = std::array<double, 4>;

struct FrameInfo {
    std::string note;
    std::size_t step = 0;
    std::size_t numSteps = 0;
};

using FrameHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                     const FrameInfo &info)>;

struct Design {
    std::vector<Cell> cells;
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    constraintMgr fences;
    FrameHook onFrame;
};

}  // namespace ktplace::ntuplace1
