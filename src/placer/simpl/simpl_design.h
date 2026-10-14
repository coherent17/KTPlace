// @file simpl_design.h
// The design as SimPL sees it.
//
// Deliberately not the shared Graph: SimPL is its own library and knows
// nothing about ktDM. The adaptor in src/adaptor fills one of these from the
// shared database, SimPL works on it alone, and its answer goes back through
// the adaptor.

#pragma once

#include "placer/simpl/simpl_fence.h"
#include "placer/simpl/simpl_graph.h"

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ktplace::simpl {

/// One placement region of a row. Coordinates are absolute and already resolved
/// from site units by the adaptor, so the solver never needs the pitch to read one.
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

/// One snapshot the solver wants drawn. SimPL decides *when* a frame matters;
/// the host decides how to draw it, which is what keeps the solver free of any
/// plotting code.
struct FrameInfo {
    std::string note;
    /// Which series of snapshots this is, for the file name.
    std::string series;
    std::size_t step = 0;
    std::size_t numSteps = 0;
    double hpwl = 0.0;
    double hpwlInitial = 0.0;
    double overflow = 0.0;
    /// True for a frame that marks a milestone, which the host keeps even when
    /// it is sampling.
    bool mandatory = false;
};

/// Host hook: called with the position of every cell. Null disables drawing.
using FrameHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                     const FrameInfo &info)>;

/// One bin-density snapshot: the grid the density test is measured on, and what
/// it measured. Handed over as numbers rather than as a picture, so the solver
/// chooses no file format and the host can draw it, store it, or drop it.
struct DensityInfo {
    /// Which series of snapshots this is, for the file name.
    std::string series;
    std::size_t step = 0;
    std::string note;
    /// Overflow of this placement on this grid.
    double overflow = 0.0;
    DieBox die{};
    /// Grid origin and bin size; bin (ix, iy) starts at (x0 + ix dx, y0 + iy dy).
    double x0 = 0.0, y0 = 0.0, dx = 1.0, dy = 1.0;
    std::size_t nbx = 0, nby = 0;
    /// Cell area A_c and available area A_a per bin, row-major (iy * nbx + ix).
    std::vector<double> occ;
    std::vector<double> avail;
    /// The fixed blockages, as x, y, width, height.
    std::vector<std::array<double, 4>> blockages;
};

/// Host hook for a density snapshot. Null disables it.
using DensityHook = std::function<void(const DensityInfo &info)>;

/// HPWL of the upper and the lower bound after one global iteration.
struct BoundsPoint {
    double upper = 0.0;
    double lower = 0.0;
};

/// Host hook for the bounds of every global iteration, called once at the end of
/// the run. Null disables it.
using BoundsHook = std::function<void(const std::vector<BoundsPoint> &curve)>;

/// A whole design as SimPL sees it.
struct Design {
    Graph graph;
    std::vector<RowInfo> rows;
    DieBox die{};
    /// Empty means the design declared no regions, which is a correctly
    /// unconstrained design rather than a missing one.
    Fences fences;
    /// Installed by the host. SimPL itself never draws and never writes a file:
    /// it says when a snapshot matters and hands over the numbers.
    FrameHook onFrame;
    DensityHook onDensity;
    BoundsHook onBounds;
};

}  // namespace ktplace::simpl
