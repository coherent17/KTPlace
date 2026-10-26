#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/abacus/abacus_options.h"
#include "util/kt_packedLists.h"

#include <array>
#include <cstddef>
#include <functional>
#include <limits>
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

using Nets = PackedLists<NetPin>;

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
    std::vector<RowInfo> rows;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
    DieBox die{};
    FrameHook onFrame;
};

class abacusDM {
public:
    abacusDM(Design design, AbacusOptions options);

    [[nodiscard]] const AbacusOptions &getOptions() const noexcept {
        return opts;
    }
    [[nodiscard]] AbacusOptions &getOptions() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &getDesign() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    void run();
    [[nodiscard]] abacusSolution getSolution() const;

private:
    Design d;
    AbacusOptions opts;
};

[[nodiscard]] abacusDM buildAbacusDM(const ktDM &db);

}  // namespace ktplace::abacus
