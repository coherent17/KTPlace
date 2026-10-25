#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_options.h"

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

namespace ktplace::fastdp {

inline constexpr int noRegion = -1;
inline constexpr std::size_t noSlot = std::numeric_limits<std::size_t>::max();
inline constexpr double eps = 1e-6;

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

    [[nodiscard]] double getPitch() const noexcept {
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
    std::vector<FixedBox> blockages;
    std::size_t tallCells = 0;
    std::vector<RowInfo> rows;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
    DieBox die{};
    FrameHook onFrame;
};

class fastdpDM {
public:
    fastdpDM(Design design, FastdpOptions options);

    [[nodiscard]] const FastdpOptions &getOptions() const noexcept {
        return opts;
    }
    [[nodiscard]] FastdpOptions &getOptions() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &getDesign() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    void run();
    [[nodiscard]] fastdpSolution getSolution() const;

private:
    Design d;
    FastdpOptions opts;
};

[[nodiscard]] fastdpDM buildFastdpDM(const ktDM &db);

}  // namespace ktplace::fastdp
