// @file multirow_design.h
// The design as the multi-row legalizer sees it.
//
// Deliberately not the shared Graph. This pass slices the die into tracks and
// carves a tall cell's footprint out of them, so what it needs is cells and rows
// -- plus the netlist, for nothing but reporting the wirelength the pass cost,
// and the regions, for the one self-check it makes. The adaptor in src/adaptor
// builds one of these from the shared database; this legalizer never sees ktDM.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ktplace::multirow {

/// A cell to place: its size, where it started, and which region claims it.
/// No name and no netlist identity, because a tall cell is placed by its
/// geometry alone.
struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    /// Index into the regions below, or kNoRegion.
    int regionId = -1;
};

/// A pin on a cell. Only its offset from the cell origin matters, for HPWL.
struct Pin {
    std::size_t cellId = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// A net, kept only so the pass can report the wirelength it cost. Nothing about
/// a net constrains where a tall cell may sit.
struct Net {
    std::vector<std::size_t> pins;
};

/// Half-perimeter wirelength over a placement, for reporting. This pass does not
/// optimize wirelength -- it is trying to fit tall cells into rows -- but the cost
/// of doing so is worth reporting.
[[nodiscard]] double netlistHPWL(const std::vector<Cell> &cells, const std::vector<Net> &nets,
                                 const std::vector<Pin> &pins, const std::vector<double> &xs,
                                 const std::vector<double> &ys);

/// One placement region of a row. Coordinates are absolute and already resolved
/// from site units by the adaptor, so the pass never needs the pitch to read one.
struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
};

/// One placement row: its band and its site pitch.
struct RowInfo {
    double coordinate = 0.0;
    double height = 0.0;
    double sitePitch = 0.0;

    [[nodiscard]] double pitch() const noexcept {
        return sitePitch;
    }
    std::vector<Subrow> subrows;
};

/// The die bounding box, xlo ylo xhi yhi.
using DieBox = std::array<double, 4>;

/// One rectangle of a placement region.
struct Rect {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
};

/// One placement region: a union of rectangles, generally disconnected.
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

/// The regions of one design. Empty means a correctly unconstrained design.
///
/// Only ever asked whether a cell is where its region says it should be. There
/// is no push-out or clamp: legality here comes from the tracks, and the regions
/// are a self-check, not a constraint this pass solves against.
class Fences {
public:
    static constexpr int kNoRegion = -1;

    [[nodiscard]] std::size_t numRegions() const noexcept {
        return regions.size();
    }
    [[nodiscard]] const Region &region(int id) const {
        return regions.at(static_cast<std::size_t>(id));
    }
    void addRegion(Region r) {
        regions.push_back(std::move(r));
    }

    /// How many of `regionIds` sit outside their region, where `positions` is a
    /// flat x,y list indexed the same way.
    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

private:
    std::vector<Region> regions;
};

/// One snapshot the pass wants drawn. The legalizer decides *when* a frame
/// matters; the host decides how to draw it, which keeps it free of plotting.
struct FrameInfo {
    std::string note;
    std::size_t step = 0;
    std::size_t numSteps = 0;
    double hpwl = 0.0;
    double hpwlInitial = 0.0;
    double overflow = 0.0;
    bool mandatory = false;
    std::string path;
};

/// Host hook: called with the position of every cell. Null disables drawing.
using FrameHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                     const FrameInfo &info)>;

/// A whole design as this pass sees it.
struct Design {
    std::vector<Cell> cells;
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    Fences fences;
    /// Installed by the host. This pass never draws.
    FrameHook onFrame;
};

}  // namespace ktplace::multirow
