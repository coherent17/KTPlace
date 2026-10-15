// @file kt_visualizer.h
// The one place a run draws anything.
//
// Everything a run draws -- SVG stills, PNG/PPM rasters, and the animated GIF --
// goes through this. It holds the run's output directory and settings, tracks
// which stage is drawing, and owns the animation. A stage says what it wants
// drawn and when; it never names a file, chooses a format, or touches the GIF.
//
// A singleton because the output is a property of the run, not of any one
// caller: there is one directory, one frame budget, and one animation, and the
// stages share them. Two visualizers would mean two half-animations.
//
// The drawing itself lives in kt_visualizer.cc alongside the orchestration. The
// declarations below are what a caller needs; a stage should reach for
// beginStage() and frame() rather than these.

#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"
#include "visualization/kt_plotOptions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

/// Bounding box of a region: {xmin, ymin, xmax, ymax}.
using BBox = std::array<double, 4>;

/// 24-bit colour, matching the format GIF's palette stores.
struct Rgb24 {
    std::uint8_t r = 0, g = 0, b = 0;
    /// The three channels as an array, for filling an image a channel at a time.
    [[nodiscard]] std::array<std::uint8_t, 3> rgb() const {
        return {r, g, b};
    }
};

/// Create @p dir and its parents. @return false when it could not be created.
[[nodiscard]] bool ensureDir(const std::string &dir);

/// Bounding box of the fixed/terminal cells -- the die frame.
/// Returns {0,0,1,1} when there are no fixed cells.
[[nodiscard]] BBox fixedCellBBox(const Graph &g);

/// Encode raw RGB pixels as a PNG. @return false when the file cannot be written.
[[nodiscard]] bool writePng(const std::string &path, int w, int h,
                            const std::vector<std::uint8_t> &rgb);

/// Render one placement snapshot as an SVG image. Every cell in the graph is
/// drawn, with no decimation, at a fixed die-relative scale so a sequence of
/// frames can be compared.
void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note, const constraintMgr *constraints = nullptr,
                   bool fixedView = false, bool worldUnits = false);

/// Render one placement snapshot as an image, the CImg counterpart of
/// writeFrameSvg(). Draws the same layout; the output format follows the
/// extension of @p path.
void writeFrameRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                      const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                      std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                      const std::string &note, const constraintMgr *constraints = nullptr,
                      bool fixedView = false, double zoom = 1.0);

/// The finished placement as a high-resolution still, at @p zoom times the frame
/// resolution (768x768 at zoom 1). This is the "look at the result" image: the
/// animation frames are small by design, and a big design's cells collapse to a
/// pixel or two in them.
void writeFinalFrameRaster(const std::string &path, const Graph &g,
                           const constraintMgr *constraints = nullptr, double zoom = 4.0,
                           double hpwl = 0.0);

/// The finished placement as vector SVG: the picture to open when a region has
/// to be looked at closely, and the exact record of the drawing -- one rectangle
/// element per cell, so "every cell is in the picture" is a count.
void writeFinalFrameSvg(const std::string &path, const Graph &g,
                        const constraintMgr *constraints = nullptr, double hpwl = 0.0);

/// Assemble the per-iteration raster stills in @p dir into one animated GIF.
///
/// Self-contained: the GIF89a container, its LZW compression and the palette are
/// all written in-process, so no external tool and no external image library is
/// involved -- the result is reproducible from the ktplace binary alone. All
/// frames share one palette, which GIF requires for colours to stay stable.
///
/// Collects "frame_NNNN.ppm", which is what writeFrameRaster() produces under the
/// frame naming convention. The stills are read back from disk rather than
/// buffered, so memory stays flat however many iterations there are.
///
/// Once the GIF is written the stills are deleted: they are the encoder's scratch
/// space. If the encode fails they are left alone, since they are then the only
/// record of the run's frames.
[[nodiscard]] bool writeAnimatedGif(const std::string &dir, const std::string &gifName,
                                    int delayCs = 60);

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

struct BoundsSample {
    double upper = 0.0;
    double lower = 0.0;
};

class Visualizer {
public:
    static Visualizer &instance() noexcept;

    Visualizer(const Visualizer &) = delete;
    Visualizer &operator=(const Visualizer &) = delete;

    /// Set up the run's output: the directory, and the settings every frame
    /// shares. Called once, by the flow, before any stage runs.
    void configure(const std::string &dir, const PlotOptions &options);

    /// Say which stage is drawing from now. Frames before this go nowhere, and a
    /// stage with no cadence set draws nothing at all.
    void beginStage(StageVisual stage);

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] const PlotOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] const std::string &directory() const noexcept {
        return dir;
    }

    /// Draw one snapshot: a vector still, and a frame for the run's animation.
    /// `x` and `y` hold one entry per cell.
    void frame(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
               const std::array<double, 4> &die, const constraintMgr *fences,
               const VisualFrame &what);

    void boundsCurve(std::string_view name, std::string_view title,
                     std::span<const BoundsSample> curve);

    /// The finished placement, at the run's final resolution: a PNG, the vector
    /// form, and on request the raw raster.
    void finalImage(const Graph &graph, const constraintMgr *fences, double hpwl);

    /// Rasterise the placement as the next frame of the run. A no-op when the
    /// run is not animating, when the cap is reached, or when the positions are
    /// not one entry per cell. The cap is checked before the work, not after, so
    /// a capped run costs nothing.
    void recordFrame(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
                     const std::array<double, 4> &die, std::size_t step, std::size_t total,
                     double hpwl, double hpwlInitial, double resid, const std::string &note,
                     const constraintMgr *fences = nullptr, bool mandatory = false);

    /// Hold the finished placement on screen, then assemble the animation.
    /// Called once every stage has contributed, because an animation closed at
    /// the end of global placement stops exactly where it gets interesting.
    bool finish(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
                const std::array<double, 4> &die, const constraintMgr *fences = nullptr,
                double hpwl = 0.0);

    /// Reserve frames for the stages that come later. The stages that follow get
    /// first call on the budget, so the animation always reaches past the placer.
    void holdBack(std::size_t frames);

    /// Frames recorded so far this run.
    [[nodiscard]] std::size_t frameCount() const noexcept {
        return framesKept;
    }
    /// Frames still recordable, ignoring any hold-back.
    [[nodiscard]] std::size_t budget() const noexcept {
        return framesKept >= maxFrames ? 0 : maxFrames - framesKept;
    }
    /// True once the frame ceiling has been reached.
    [[nodiscard]] bool capped() const noexcept {
        return cappedNow;
    }
    /// Forget the current run. Frames already written are left on disk.
    void reset();

    /// True once configure() has armed this run for an animation.
    [[nodiscard]] bool animating() const noexcept {
        return animOn;
    }

private:
    Visualizer() = default;


    /// `<dir>/<subdir>/<stage>[_<series>]_%06zu.svg`. Numbered to a fixed width
    /// so a directory listing sorts in run order, not alphabetically.
    [[nodiscard]] std::string framePath(const VisualFrame &frame) const;
    [[nodiscard]] bool ensureDir(const std::string &where) const;

    std::string dir;
    PlotOptions opts;
    StageVisual stage;
    bool drawingNow = false;

    // The animation: a sequence of raster stills, assembled into one GIF at the
    // end of the run.
    bool animOn = false;
    std::filesystem::path animDir;
    int delayCs = 12;
    int blendSteps = 3;
    double frameZoom = 1.0;
    std::size_t maxFrames = 480;
    std::size_t framesKept = 0;  ///< frames kept so far
    std::size_t offered = 0;     ///< offered since the last reset
    std::size_t stride = 1;      ///< spacing between the kept ones
    std::size_t held = 0;        ///< reserved for later stages
    bool cappedNow = false;
    /// The previous recorded placement, kept only to interpolate from.
    std::vector<float> prevX;
    std::vector<float> prevY;
    bool havePrev = false;
};

}  // namespace ktplace
