// @file abacus_design.h
// The design as Abacus sees it.
//
// Deliberately not the shared Graph: Abacus is a legalizer and thinks in rows,
// tracks and clusters, and this is only what that needs. The adaptor in
// src/adaptor builds one of these from the shared database; Abacus never sees
// ktDM.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ktplace::abacus {

/// A cell to place. Abacus needs its size and where it started; it does not need
/// a name, a netlist identity, or a region, because legality here is defined
/// entirely by rows.
struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
};

/// A pin on a cell. Only its offset from the cell origin matters, for HPWL.
struct Pin {
    std::size_t cellId = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// A net, kept only so the legalizer can report the wirelength it cost. Nothing
/// about a net constrains where a cell may sit, so this is the whole of what
/// legalization needs from the netlist.
struct Net {
    std::vector<std::size_t> pins;
};

/// One placement region of a row. Coordinates are absolute and already resolved
/// from site units by the adaptor, so the legalizer never needs the pitch to
/// read one.
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
/// Abacus only ever asks whether a cell is where its region says it should be.
/// It has no push-out or clamp, because legality here comes from the rows; the
/// regions are a check, not a constraint it solves against.
class Fences {
public:
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

/// One snapshot the legalizer wants drawn. Abacus decides *when* a frame matters;
/// the host decides how to draw it, which keeps the legalizer free of plotting.
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

/// A whole design as Abacus sees it.
struct Design {
    std::vector<Cell> cells;
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    Fences fences;
    /// Installed by the host. Abacus itself never draws.
    FrameHook onFrame;
};

}  // namespace ktplace::abacus
