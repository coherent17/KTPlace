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
#include <span>
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

struct BoundsPoint {
    double upper = 0.0;
    double lower = 0.0;
};

using BoundsHook = std::function<void(std::span<const BoundsPoint> curve)>;

struct ProgressInfo {
    /// "init" for the warm-up solves, "global" for the LSS/LAL iterations.
    std::string phase;
    std::size_t iter = 0;
    std::size_t total = 0;
    double hpwl = 0.0;
    double hpwlLower = 0.0;
    double overflow = 0.0;
};

/// Called once per outer iteration. `xs`/`ys` hold every cell's position when
/// Design::progressPositions is set, and are empty otherwise.
using ProgressHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                        const ProgressInfo &info)>;

/// A whole design as SimPL sees it.
struct Design {
    Graph graph;
    std::vector<RowInfo> rows;
    DieBox die{};
    /// Empty means the design declared no regions, which is a correctly
    /// unconstrained design rather than a missing one.
    Fences fences;
    /// Installed by the host. SimPL itself never draws.
    FrameHook onFrame;
    BoundsHook onBounds;
    ProgressHook onProgress;
    bool progressPositions = false;
    /// Asked between outer iterations; true ends the run with the placement so far.
    std::function<bool()> stopRequested;
};

}  // namespace ktplace::simpl
