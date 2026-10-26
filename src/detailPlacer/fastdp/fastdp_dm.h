#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_options.h"
#include "geometry/kt_geometry.h"
#include "util/kt_packedLists.h"

#include <cstddef>
#include <functional>
#include <limits>
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

using Nets = PackedLists<NetPin>;

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
    std::vector<BBox> fixedBoxes;
    std::vector<BBox> blockages;
    std::size_t tallCells = 0;
    std::vector<RowInfo> rows;
    std::vector<double> vertexX;
    std::vector<double> vertexY;
    BBox die{};
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
