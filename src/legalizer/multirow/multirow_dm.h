#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_options.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::multirow {

struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    int regionId = -1;
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

inline constexpr int noRegion = -1;

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

struct Rect {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
};

struct Region {
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
    static constexpr int noRegion = multirow::noRegion;

    [[nodiscard]] std::size_t numRegions() const noexcept {
        return regions.size();
    }
    [[nodiscard]] const Region &region(int id) const {
        return regions.at(static_cast<std::size_t>(id));
    }
    void addRegion(Region r) {
        regions.push_back(std::move(r));
    }

    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

private:
    std::vector<Region> regions;
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
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    Fences fences;
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
