#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/simpl/simpl_grid.h"
#include "globalPlacer/simpl/simpl_lss.h"
#include "globalPlacer/simpl/simpl_options.h"

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

namespace ktplace::simpl {

struct Rect {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
    [[nodiscard]] double getWidth() const noexcept {
        return xhi - xlo;
    }
    [[nodiscard]] double getHeight() const noexcept {
        return yhi - ylo;
    }
};

struct Region {
    std::string name;
    std::vector<Rect> rects;
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;

    [[nodiscard]] bool contains(double x, double y) const {
        if (x < minX || x > maxX || y < minY || y > maxY) {
            return false;
        }
        for (const Rect &r : rects) {
            if (r.contains(x, y)) {
                return true;
            }
        }
        return false;
    }
    void seal();
};

class Fences {
public:
    static constexpr int noRegion = -1;

    [[nodiscard]] std::size_t getNumRegions() const noexcept {
        return regions.size();
    }
    [[nodiscard]] const Region &getRegion(int id) const {
        return regions.at(static_cast<std::size_t>(id));
    }

    bool clampToRegion(int id, double &x, double &y) const;

    bool pushOutOfRegions(double &x, double &y, double dieMinX, double dieMinY, double maxX,
                          double maxY) const;

    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

    void addRegion(Region r) {
        regions.push_back(std::move(r));
    }

private:
    std::vector<Region> regions;
};

struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
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

struct BoundsPoint {
    double upper = 0.0;
    double lower = 0.0;
};

using BoundsHook = std::function<void(std::span<const BoundsPoint> curve)>;

struct Design {
    Netlist netlist;
    std::vector<double> width;
    std::vector<double> height;
    std::vector<double> area;
    std::vector<int> regionOf;
    std::vector<Box> obstacles;
    std::size_t numFixed = 0;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<RowInfo> rows;
    DieBox die{};
    Fences fences;
    FrameHook onFrame;
    BoundsHook onBounds;
};

struct SimplResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;

    std::size_t initIters = 0;
    std::size_t globalIters = 0;

    std::size_t binsX = 0;
    std::size_t binsY = 0;

    double hpwlSeed = 0.0;
    double hpwlInit = 0.0;
    double hpwlLower = 0.0;
    double hpwlFinal = 0.0;
    double gap = 0.0;

    double overflowLower = 0.0;
    double overflowFinal = 0.0;

    std::size_t fenceRegions = 0;
    std::size_t fenceClamps = 0;
    std::size_t fencePushes = 0;
    std::size_t fenceViolations = 0;

    double spreadSeconds = 0.0;
    double buildSeconds = 0.0;
    double solveSeconds = 0.0;
};


class simplDM {
public:
    simplDM(Design design, SimplOptions options);

    [[nodiscard]] const SimplOptions &getOptions() const noexcept {
        return opts;
    }
    [[nodiscard]] SimplOptions &getOptions() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &getDesign() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }
    void setBoundsHook(BoundsHook hook) {
        d.onBounds = std::move(hook);
    }

    SimplResult run();
    [[nodiscard]] simplSolution getSolution() const;

private:
    Design d;
    SimplOptions opts;
};

[[nodiscard]] simplDM buildSimplDM(const ktDM &db);

}  // namespace ktplace::simpl
