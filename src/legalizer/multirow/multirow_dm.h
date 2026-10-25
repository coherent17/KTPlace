#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::multirow {

inline constexpr int noRegion = -1;

struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    int region = noRegion;
    bool movable = false;
};

struct NetPin {
    std::size_t cellId = 0;
    double offX = 0.0;
    double offY = 0.0;
};

class Nets {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return firstPin.size() - 1;
    }
    [[nodiscard]] std::span<const NetPin> operator[](std::size_t net) const {
        return std::span(pins).subspan(firstPin[net], firstPin[net + 1] - firstPin[net]);
    }
    void add(std::span<const NetPin> netPins) {
        pins.insert(pins.end(), netPins.begin(), netPins.end());
        firstPin.push_back(pins.size());
    }

private:
    std::vector<std::size_t> firstPin{0};
    std::vector<NetPin> pins;
};

struct Box {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
    int region = noRegion;
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

struct Design {
    std::vector<Cell> cells;
    Nets nets;
    std::vector<Box> blockages;
    std::vector<RowInfo> rows;
    DieBox die{};
    FrameHook onFrame;
};

struct MultiRowResult {
    std::size_t movable = 0;
    std::size_t placed = 0;
    std::size_t unplaced = 0;
    std::size_t multiRow = 0;
    std::size_t multiRowPlaced = 0;
    std::size_t macros = 0;
    std::size_t macrosPlaced = 0;
    double displacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

class multirowDM {
public:
    multirowDM(Design design, MultiRowOptions options);

    [[nodiscard]] const MultiRowOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] MultiRowOptions &options() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    MultiRowResult run();
    [[nodiscard]] multiRowSolution solution() const;

private:
    Design d;
    MultiRowOptions opts;
};

[[nodiscard]] multirowDM buildMultiRowDM(const ktDM &db);

}  // namespace ktplace::multirow
