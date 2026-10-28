#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "geometry/kt_geometry.h"
#include "legalizer/ripplelg/ripplelg_options.h"
#include "util/kt_packedLists.h"

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::ripplelg {

inline constexpr int noRegion = -1;

struct Cell {
    Point position;
    double width = 0.0;
    double height = 0.0;
    int region = noRegion;
    bool isMovable = false;
};

struct NetPin {
    std::size_t cellId = 0;
    Point offset;
};

using Nets = PackedLists<NetPin>;

struct Subrow {
    Interval x;
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
    std::vector<BBox> blockages;
    std::vector<RowInfo> rows;
    BBox die;
    FrameHook onFrame;
};

class ripplelgDM {
public:
    ripplelgDM(Design design, RipplelgOptions options);

    [[nodiscard]] const RipplelgOptions &getOptions() const noexcept {
        return opts;
    }
    [[nodiscard]] RipplelgOptions &getOptions() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &getDesign() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    void run();
    [[nodiscard]] ripplelgSolution getSolution() const;

private:
    Design d;
    RipplelgOptions opts;
};

[[nodiscard]] ripplelgDM buildRipplelgDM(const ktDM &db);

}  // namespace ktplace::ripplelg
