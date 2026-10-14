// @file kt_frameSink.h
// Where a run's snapshots go
//
// The flow hands every snapshot a stage offers to one FrameSink. Drawing it on
// the spot (Visualizer) and recording it to a trace for later (TraceWriter) are
// two sinks behind the same calls, so the flow does not know which one it has,
// and a trace replayed into a Visualizer draws what the run would have drawn.

#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace ktplace {

/// One snapshot a stage wants drawn.
///
/// A stage fills this in and hands it over; it does not say where the file goes
/// or what it is called.
struct VisualFrame {
    /// Which series of snapshots this belongs to, for the file name: a stage that
    /// produces more than one kind of frame says which is which ("LSS", "LAL").
    std::string series;
    /// Which step of the stage's own loop this is, and how many there are.
    std::size_t step = 0;
    std::size_t numSteps = 0;
    /// What the placement looked like, for the frame's caption.
    std::string note;
    double hpwl = 0.0;
    double hpwlInitial = 0.0;
    double overflow = 0.0;
    /// A milestone worth keeping even when the run is sampling frames.
    bool mandatory = false;
};

/// A bin-density field, for the heat map drawn beside a placement frame. Plain
/// numbers: the stage that measured them does not say how they are drawn.
struct DensityField {
    std::string series;
    std::size_t step = 0;
    std::string note;
    double overflow = 0.0;
    std::array<double, 4> die{};
    /// Grid origin and bin size; bin (ix, iy) starts at (x0 + ix dx, y0 + iy dy).
    double x0 = 0.0, y0 = 0.0, dx = 1.0, dy = 1.0;
    std::size_t nbx = 0, nby = 0;
    /// Cell area and available area per bin, row-major (iy * nbx + ix).
    std::vector<double> occ;
    std::vector<double> avail;
    /// Fixed blockages, as x, y, width, height.
    std::vector<std::array<double, 4>> blockages;
};

/// HPWL of the upper and the lower bound after one iteration of a placer that
/// keeps both.
struct BoundsSample {
    double upper = 0.0;
    double lower = 0.0;
};

/// What a whole stage draws with: its name, and where under the output directory
/// its frames land. Set once when the stage starts.
struct StageVisual {
    std::string stage;
    /// Sub-directory, when a stage keeps its frames apart from the others.
    std::string subdir;
    /// This stage's own cadence: one frame in every N of its steps. The stage
    /// knows how much work it is doing and a visualizer is not in a position to
    /// judge that, so it is the stage's to set.
    std::size_t cadence = 0;
};

/// Everything a run does with its snapshots, in the order the flow does it.
class FrameSink {
public:
    virtual ~FrameSink() = default;

    /// Say which stage is offering snapshots from now.
    virtual void beginStage(StageVisual stage) = 0;

    /// One snapshot of the placement. `x` and `y` hold one entry per cell.
    virtual void frame(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
                       const std::array<double, 4> &die, const constraintMgr *fences,
                       const VisualFrame &what) = 0;

    /// A bin-density field, under the current stage.
    virtual void densityMap(const DensityField &field) = 0;

    /// The two bounds of every iteration of a placer that keeps both.
    virtual void boundsCurve(const std::string &name, const std::string &title,
                             const std::vector<BoundsSample> &curve) = 0;

    /// Reserve that many frames of the budget for the stages still to come.
    virtual void holdBack(std::size_t frames) = 0;

    /// The finished placement, which is the graph's committed positions.
    virtual void finalImage(const Graph &graph, const constraintMgr *fences, double hpwl) = 0;

    /// Close the run's animation. True when one was written.
    virtual bool finish(const Graph &graph, const std::vector<float> &x,
                        const std::vector<float> &y, const std::array<double, 4> &die,
                        const constraintMgr *fences, double hpwl) = 0;

    /// Whether snapshots go anywhere, and whether the run animates. A stage's
    /// snapshot cadence can depend on the second, so a recorded run answers it
    /// as the drawing run would.
    [[nodiscard]] virtual bool enabled() const = 0;
    [[nodiscard]] virtual bool animating() const = 0;
};

}  // namespace ktplace
