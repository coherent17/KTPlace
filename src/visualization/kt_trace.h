// @file kt_trace.h
// Recording a run's snapshots, and drawing them later
//
// Drawing during the run puts every SVG, raster and the GIF on the solver's
// critical path. A trace takes the same calls the flow makes on a Visualizer and
// appends them to one compressed file instead, so a run pays for a copy of the
// positions and nothing else; `ktplace-viz` replays the file into a Visualizer
// afterwards, as often as wanted and with other picture settings if asked.
//
// Replaying a trace makes the same calls, in the same order, with the same
// values, so it draws exactly what the run would have drawn.
//
// The file is gzip, holding:
//
//     "KTTRACE\0"  u32 version
//     records, each a one-byte tag and its payload:
//       'O' picture settings       'N' cell geometry      'R' fence rectangles
//       'S' stage begins           'F' frame              'D' density field
//       'B' bounds curve           'H' frames held back   'I' final image
//       'E' animation closes       'Z' end of trace
//
// Little-endian, fixed-width fields; a string is a u32 length and its bytes, an
// array a u64 count and its elements. A trace without 'Z' is from a run that did
// not finish: it is drawn up to where it stops, with a warning.

#pragma once

#include "visualization/kt_frameSink.h"
#include "visualization/kt_plotOptions.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

class Visualizer;

/// Version of the format this build reads and writes.
inline constexpr std::uint32_t kTraceVersion = 1;

/// Records a run's snapshots instead of drawing them.
class TraceWriter final : public FrameSink {
public:
    /// Opens `path` for writing and records the picture settings. A file that
    /// cannot be opened is fatal: a run asked to record that records nothing
    /// would be worse than one that stops.
    TraceWriter(const std::string &path, const PlotOptions &options);
    /// Closes the file. Without a close() first the trace is left unmarked, which
    /// is how a run that stopped part-way reads when it is drawn.
    ~TraceWriter() override;

    TraceWriter(const TraceWriter &) = delete;
    TraceWriter &operator=(const TraceWriter &) = delete;

    void beginStage(StageVisual stage) override;
    void frame(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
               const std::array<double, 4> &die, const constraintMgr *fences,
               const VisualFrame &what) override;
    void densityMap(const DensityField &field) override;
    void boundsCurve(const std::string &name, const std::string &title,
                     const std::vector<BoundsSample> &curve) override;
    void holdBack(std::size_t frames) override;
    void finalImage(const Graph &graph, const constraintMgr *fences, double hpwl) override;
    /// Records the close of the animation; whether one gets written is decided
    /// when the trace is drawn, so this always answers false.
    bool finish(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
                const std::array<double, 4> &die, const constraintMgr *fences,
                double hpwl) override;

    [[nodiscard]] bool enabled() const noexcept override {
        return true;
    }
    /// What a drawing run would answer, so a stage's cadence does not depend on
    /// whether its snapshots are drawn now or later.
    [[nodiscard]] bool animating() const noexcept override {
        return animate;
    }

    /// Placement snapshots recorded so far.
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return frames;
    }
    [[nodiscard]] const std::string &path() const noexcept {
        return file;
    }

    /// Mark the trace complete and close it. Nothing may be recorded after.
    void close();

private:
    class Out;
    /// The cell geometry, and the fences, the first time each is seen. One
    /// netlist per trace: every stage draws the flow's own database.
    void describe(const Graph &graph, const constraintMgr *fences);

    std::unique_ptr<Out> out;
    std::string file;
    bool animate = false;
    bool haveNetlist = false;
    bool haveFences = false;
    std::size_t frames = 0;
};

/// What drawing a trace did.
struct TraceReplay {
    std::size_t frames = 0;         ///< placement snapshots drawn
    bool complete = false;          ///< the trace ended with its end marker
    bool animationWritten = false;  ///< the GIF was assembled
    PlotOptions options;            ///< the settings it was drawn with
};

/// Draw the trace at `path` into `outDir`, through `viz`, with the picture
/// settings the run recorded. `adjust`, when given, may change those settings
/// before anything is drawn -- a bigger final still, no animation. A file that
/// is not a trace, or is from another version, is fatal.
TraceReplay replayTrace(const std::string &path, const std::string &outDir, Visualizer &viz,
                        const std::function<void(PlotOptions &)> &adjust = {});

}  // namespace ktplace
