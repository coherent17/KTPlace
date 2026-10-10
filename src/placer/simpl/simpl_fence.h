// @file simpl_fence.h
// SimPL's own placement regions.
//
// The shared constraintMgr carries DEF regions with names, cell counts and
// history SimPL never looks at. This is the part it needs.

#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace ktplace::simpl {

struct Rect {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
    [[nodiscard]] double width() const noexcept {
        return xhi - xlo;
    }
    [[nodiscard]] double height() const noexcept {
        return yhi - ylo;
    }
};

/// One placement region. A region is a union of rectangles and is generally
/// disconnected, so the rectangles are kept apart rather than merged.
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
    /// Recompute the bounding box and area from the rectangles.
    void seal();
};

/// The regions of one design. Default-constructed means "no regions", which is a
/// correctly unconstrained design, not a missing one.
class Fences {
public:
    static constexpr int kNoRegion = -1;

    [[nodiscard]] std::size_t numRegions() const noexcept {
        return regions.size();
    }
    [[nodiscard]] const Region &region(int id) const {
        return regions.at(static_cast<std::size_t>(id));
    }

    /// Pull (x,y) back inside region `id`, if it is outside. Returns true when it
    /// had to move.
    bool clampToRegion(int id, double &x, double &y) const;

    /// Move (x,y) just outside every region it sits inside, when the design is
    /// unconstrained apart from fences. Returns true when it had to move.
    bool pushOutOfRegions(double &x, double &y, double dieMinX, double dieMinY, double maxX,
                          double maxY) const;

    /// How many of `regionIds` sit outside their region. `positions` is a flat
    /// x,y list indexed the same way as regionIds.
    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

    void addRegion(Region r) {
        regions.push_back(std::move(r));
    }

private:
    std::vector<Region> regions;
};

}  // namespace ktplace::simpl
