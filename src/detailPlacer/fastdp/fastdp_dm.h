#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::fastdp {

inline constexpr int noRegion = -1;

struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    int region = noRegion;
};

struct Pin {
    std::size_t cellId = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

struct Net {
    std::vector<std::size_t> pins;
};

[[nodiscard]] double netlistHPWL(const std::vector<Cell> &cells, const std::vector<Net> &nets,
                                 const std::vector<Pin> &pins, const std::vector<double> &xs,
                                 const std::vector<double> &ys);

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
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    FrameHook onFrame;
};

struct FastdpResult {
    std::size_t globalSwaps = 0;
    std::size_t verticalSwaps = 0;
    std::size_t reorderMoves = 0;
    std::size_t clusterMoves = 0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    double seconds = 0.0;
    double globalSeconds = 0.0;
    double verticalSeconds = 0.0;
    double reorderSeconds = 0.0;
    double clusterSeconds = 0.0;
};

class fastdpDM {
public:
    fastdpDM(Design design, FastdpOptions options);

    [[nodiscard]] const FastdpOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] FastdpOptions &options() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    FastdpResult run();
    [[nodiscard]] fastdpSolution solution() const;

private:
    Design d;
    FastdpOptions opts;
};

[[nodiscard]] fastdpDM buildFastdpDM(const ktDM &db);

}  // namespace ktplace::fastdp
