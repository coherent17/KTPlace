#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/abacus/abacus_options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::abacus {

inline constexpr int noRegion = -1;
inline constexpr std::size_t noSlot = std::numeric_limits<std::size_t>::max();

struct Cells {
    std::vector<std::size_t> id;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<double> width;
    std::vector<double> height;
    std::vector<int> region;

    [[nodiscard]] std::size_t size() const noexcept {
        return id.size();
    }
};

struct NetPin {
    std::size_t slot = noSlot;
    double offX = 0.0;
    double offY = 0.0;
    double absX = 0.0;
    double absY = 0.0;
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

struct FixedBox {
    double y0 = 0.0;
    double y1 = 0.0;
    double x0 = 0.0;
    double x1 = 0.0;
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
    Cells cells;
    Nets nets;
    std::vector<FixedBox> fixedBoxes;
    std::vector<RowInfo> rows;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
    DieBox die{};
    FrameHook onFrame;
};

struct AbacusResult {
    std::size_t cellsPlaced = 0;
    std::size_t unplaced = 0;
    double totalSquaredDisplacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    std::size_t outOfRows = 0;
    std::size_t commitFailures = 0;
};

class abacusDM {
public:
    abacusDM(Design design, AbacusOptions options);

    [[nodiscard]] const AbacusOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] AbacusOptions &options() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    AbacusResult run();
    [[nodiscard]] abacusSolution solution() const;

private:
    Design d;
    AbacusOptions opts;
};

[[nodiscard]] abacusDM buildAbacusDM(const ktDM &db);

}  // namespace ktplace::abacus
