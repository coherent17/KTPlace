#pragma once

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_solutionMgr.h"
#include "geometry/kt_geometry.h"
#include "globalPlacer/ntuplace1/ntuplace1_options.h"

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
    std::vector<std::size_t> id;
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
    BBox fixedPinBox = BBox::getEmpty();
    std::size_t fixedPins = 0;
};

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
    std::vector<BBox> fixedRects;
    std::vector<BBox> rowRects;
    BBox rowBox{};
    double maxRowH = 0.0;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
    BBox die{};
    constraintMgr fences;
    FrameHook onFrame;
};

class ntuplace1DM {
public:
    ntuplace1DM(Design design, Ntuplace1Options options);

    [[nodiscard]] const Ntuplace1Options &getOptions() const noexcept {
        return opts;
    }
    [[nodiscard]] Ntuplace1Options &getOptions() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &getDesign() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    void run();
    [[nodiscard]] ntuplace1Solution getSolution() const;

private:
    Design d;
    Ntuplace1Options opts;
};

[[nodiscard]] ntuplace1DM buildNtuplace1DM(const ktDM &db);

}  // namespace ktplace::ntuplace1
