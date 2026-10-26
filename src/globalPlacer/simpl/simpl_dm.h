#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "geometry/kt_geometry.h"
#include "globalPlacer/simpl/simpl_grid.h"
#include "globalPlacer/simpl/simpl_lss.h"
#include "globalPlacer/simpl/simpl_options.h"

#include <algorithm>
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

struct Region {
    std::string name;
    std::vector<BBox> rects;
    BBox bounds;

    [[nodiscard]] bool contains(const Point &p) const {
        return bounds.contains(p) && std::ranges::any_of(rects, [&](const BBox &r) {
                   return r.contains(p);
               });
    }
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

    bool clampToRegion(int id, Point &p) const;

    bool pushOutOfRegions(Point &p, const BBox &die) const;

    [[nodiscard]] std::size_t countViolations(const std::vector<Point> &positions,
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
    std::vector<int> region;
    std::vector<BBox> obstacles;
    std::size_t numFixed = 0;
    std::vector<double> x;
    std::vector<double> y;
    std::vector<RowInfo> rows;
    BBox die{};
    Fences fences;
    FrameHook onFrame;
    BoundsHook onBounds;
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

    void run();
    [[nodiscard]] simplSolution getSolution() const;

private:
    Design d;
    SimplOptions opts;
};

[[nodiscard]] simplDM buildSimplDM(const ktDM &db);

}  // namespace ktplace::simpl
