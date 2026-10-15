// @file kt_trace.h
// Recording a run's snapshots while it places, and drawing them once it is done
//
// The flow hands every snapshot to a TraceWriter, which appends it to one
// compressed file, so the placer's loops pay for a copy of the positions and
// nothing else. At the end of the flow replayTrace() makes the same Visualizer
// calls, in the same order, with the same values, so the pictures are the ones
// drawing during the run would have made.
//
// The file is gzip, holding records, each a one-byte tag and its payload:
//
//     'S' stage begins    'F' frame    'B' bounds curve    'H' frames held back
//
// Little-endian, fixed-width fields; a string is a u32 length and its bytes, an
// array a u64 count and its elements. The netlist, the fences and the final
// placement are not recorded: the flow still has them when it draws.

#pragma once

#include "visualization/kt_visualizer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

class TraceWriter {
public:
    /// A file that cannot be opened is fatal: a run that silently drew nothing
    /// would be worse than one that stops.
    explicit TraceWriter(const std::string &path);
    ~TraceWriter();

    TraceWriter(const TraceWriter &) = delete;
    TraceWriter &operator=(const TraceWriter &) = delete;

    void beginStage(const StageVisual &stage);
    void frame(const std::vector<float> &x, const std::vector<float> &y,
               const std::array<double, 4> &die, bool withFences, const VisualFrame &what);
    void boundsCurve(std::string_view name, std::string_view title,
                     std::span<const BoundsSample> curve);
    void holdBack(std::size_t frames);

    /// Flush and close the file; nothing may be recorded after.
    void close();

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return frames;
    }
    [[nodiscard]] const std::string &path() const noexcept {
        return file;
    }

private:
    class Out;
    std::unique_ptr<Out> out;
    std::string file;
    std::size_t frames = 0;
};

/// Draw the closed trace at `path` through `viz`, which the caller has
/// configured. `graph` and `fences` are the netlist the frames were taken of.
/// Returns the number of frames drawn. A record cut short is fatal.
std::size_t replayTrace(const std::string &path, Visualizer &viz, const Graph &graph,
                        const constraintMgr *fences);

}  // namespace ktplace
