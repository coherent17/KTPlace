#pragma once

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/ntuplace1/ntuplace1_options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::ntuplace1 {

struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    int region = constraintMgr::noRegion;
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

struct Ntuplace1Result {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;
    std::size_t cuts = 0;
    std::size_t ratioRetries = 0;
    std::size_t maxDepth = 0;
    std::size_t minLeafCells = 0;
    double hpwlFinal = 0.0;
    double meanImbalance = 0.0;
    std::size_t fenceRegions = 0;
    std::size_t fenceViolations = 0;
};

class ntuplace1DM {
public:
    ntuplace1DM(Design design, Ntuplace1Options options);

    [[nodiscard]] const Ntuplace1Options &options() const noexcept {
        return opts;
    }
    [[nodiscard]] Ntuplace1Options &options() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    Ntuplace1Result run();
    [[nodiscard]] ntuplace1Solution solution() const;

private:
    Design d;
    Ntuplace1Options opts;
};

[[nodiscard]] ntuplace1DM buildNtuplace1DM(const ktDM &db);

}  // namespace ktplace::ntuplace1
