#pragma once

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/ntuplace1/ntuplace1_options.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::ntuplace1 {

struct Cells {
    std::vector<std::uint32_t> id;
    std::vector<double> width;
    std::vector<double> height;
    std::vector<double> area;
    std::vector<int> region;
    std::vector<double> x;
    std::vector<double> y;

    [[nodiscard]] std::size_t size() const noexcept {
        return id.size();
    }
};

struct HyperNet {
    std::vector<std::uint32_t> cells;
    double fx0 = 0.0;
    double fx1 = 0.0;
    double fy0 = 0.0;
    double fy1 = 0.0;
    std::size_t fixedPins = 0;
};

struct AreaRect {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
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
    Cells cells;
    std::vector<HyperNet> hyperNets;
    std::vector<std::vector<std::uint32_t>> cellNets;
    std::vector<AreaRect> fixedRects;
    std::vector<AreaRect> rowRects;
    std::array<double, 4> rowBox{};
    double maxRowH = 0.0;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
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
