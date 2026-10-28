#include "visualization/kt_visualizer.h"

#include "datamodel/kt_routingDemand.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_gif.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <span>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <zlib.h>

// CImg is a header-only library, so it is confined to this translation unit.
// Drawing is done in RGB and then mapped onto a small fixed palette, which is
// what GIF requires and what keeps every frame of an animation colour-identical.
#define cimg_display 0
#define cimg_use_pthread 1
#define cimg_verbosity 0
#include "CImg.h"

using cimg_library::CImg;
using cimg_library::CImgList;

namespace ktplace {
namespace {

constexpr const char *animationName = "placement.gif";
constexpr const char *demandAnimationName = "routing_demand.gif";
constexpr const char *finalNote = "Detailed placement result";
constexpr const char *finalImageName = "placement.png";
constexpr std::size_t pngBands = 64;
constexpr int pngLevel = 6;

/// Zero-padded so directory order is frame order. Matches the GIF writer's
/// expectation of "frame_NNNN.ppm".
std::string getFrameName(std::size_t n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) {
        s.insert(s.begin(), '0');
    }
    return "frame_" + s + ".ppm";
}

}  // namespace

Visualizer &Visualizer::getInstance() noexcept {
    static Visualizer viz;
    return viz;
}

bool Visualizer::ensureDir(const std::string &where) const {
    if (where.empty()) {
        return true;  // nothing to make: the run asked for no output
    }
    // create_directories without an error_code throws, which would turn a
    // plotting problem into a lost placement, so a failure is reported and the
    // run carries on without output.
    std::error_code ec;
    std::filesystem::create_directories(where, ec);
    if (ec) {
        ktlog.warning("cannot create the plot directory '{}': {}. Continuing without output.",
                      where, ec.message());
        return false;
    }
    return true;
}

void Visualizer::render(std::function<void()> job) {
    if (pending.load() >=
        2 * static_cast<std::size_t>(std::max(1u, std::thread::hardware_concurrency()))) {
        flush();
    }
    ++pending;
    renders.run([this, job = std::move(job)] {
        job();
        --pending;
    });
}

void Visualizer::flush() {
    renders.wait();
}

void Visualizer::reset() {
    flush();
    animOn = false;
    framesKept = offered = held = 0;
    stride = 1;
    cappedNow = false;
    havePrev = false;
    prevX.clear();
    prevY.clear();
    drawingNow = false;
}

void Visualizer::configure(const std::string &outDir, const PlotOptions &options) {
    flush();
    this->dir = outDir;
    this->opts = options;
    // The directory the run was pointed at wins over whatever the options
    // carried, so the stage sub-directories below resolve against it rather than
    // against a setting nobody set.
    this->opts.dir = outDir;
    this->stage = StageVisual{};
    drawingNow = false;
    (void)ensureDir(outDir);

    animOn = false;
    framesKept = offered = held = 0;
    stride = 1;
    cappedNow = false;
    havePrev = false;
    if (!isEnabled() || !opts.animate) {
        return;
    }

    // Budget by bytes, not by framesKept count. Placement frames are nearly
    // incompressible, so size is essentially w * h * frames / 8: a framesKept count
    // alone allowed an 8 GB file. Sizing to a byte target keeps the resolution
    // and cuts the framesKept count instead.
    const double animW = 768.0 * opts.frameZoom;
    constexpr double bytesPerPixel = 0.9 / 8.0;  // measured, not assumed
    const auto sizeCapFrames = static_cast<std::size_t>(static_cast<double>(opts.gifByteCap) /
                                                        (animW * animW * bytesPerPixel));
    // One is the floor: a zero budget would make the recorder dead rather than
    // small, and "no frames" is not the same as "no frame possible". Anything
    // above one honours the request, so a byte cap that leaves room for only a
    // handful is respected rather than quietly rounded up.
    maxFrames = std::max<std::size_t>(1, std::min(opts.frameBudget, sizeCapFrames));
    animDir = opts.getSub("frames");
    (void)ensureDir(animDir.string());
    demandDir = opts.congestionBins > 0 ? std::filesystem::path(opts.getSub("frames_demand"))
                                        : std::filesystem::path();
    if (!demandDir.empty()) {
        (void)ensureDir(demandDir.string());
    }
    delayCs = opts.frameDelayCs;
    blendSteps = opts.blendFrames;
    frameZoom = opts.frameZoom;
    animOn = true;
}

void Visualizer::beginStage(StageVisual s) {
    flush();
    stage = std::move(s);
    drawingNow = false;
    if (!isEnabled() || stage.stage.empty()) {
        return;
    }
    drawingNow =
        !opts.svgFrames || ensureDir(dir + (stage.subdir.empty() ? "" : "/" + stage.subdir));
}

bool Visualizer::isEnabled() const noexcept {
    return !dir.empty();
}

void Visualizer::holdBack(std::size_t frames) {
    if (!animOn) {
        return;
    }
    // Never reserve more than exists; a reservation that cannot be spent would
    // silently disable the stage it was meant to protect.
    held = frames < maxFrames ? frames : maxFrames / 2;
    cappedNow = false;
}

std::string Visualizer::getFramePath(const VisualFrame &shot) const {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06zu", shot.step);
    return dir + (stage.subdir.empty() ? "" : "/" + stage.subdir) + "/" + stage.stage +
           (shot.series.empty() ? "" : "_" + shot.series) + "_" + buf + ".svg";
}

std::string Visualizer::getHeading(const std::string &note) const {
    if (note == finalNote) {
        return "Final placement";
    }
    static const std::map<std::string, std::string> titles{
        {"simpl", "Global placement - SimPL"},
        {"ntuplace1", "Global placement - NTUPlace"},
        {"legalize", "Legalization - Abacus"},
        {"legalize_multi", "Legalization - multi-row"},
        {"legalize_tetris", "Legalization - Tetris"},
        {"legalize_ripple", "Legalization - RippleLG"},
        {"detailplace", "Detailed placement - FastDP"},
    };
    const auto found = titles.find(stage.stage);
    return found != titles.end() ? found->second : stage.stage;
}

void Visualizer::frame(const Graph &graph, const std::vector<float> &x, const std::vector<float> &y,
                       const BBox &die, const constraintMgr *fences, const VisualFrame &what) {
    if (!drawingNow) {
        return;
    }
    const BBox &dieBox = die;

    // Every framesKept of a sequence is drawn at the same die-relative scale, so step
    // 7 and step 8 are comparable instead of being auto-zoomed apart.
    if (opts.svgFrames) {
        render([path = getFramePath(what), &graph, x, y, dieBox, what, fences] {
            writeFrameSvg(path, graph, x, y, dieBox, what.step, what.numSteps, what.hpwl,
                          what.hpwlInitial, what.overflow, what.note, fences, /*fixedView=*/true);
        });
    }

    // And into the run's animation, so the GIF shows what this stage did rather
    // than cutting straight from the stage before to the stage after.
    if (animOn) {
        record(graph, x, y, dieBox, what.step, what.numSteps, what.hpwl, what.hpwlInitial,
               what.overflow, what.note, fences, what.mandatory, true);
    }
}

void Visualizer::boundsCurve(std::string_view name, std::string_view title,
                             std::span<const BoundsSample> curve) {
    if (!isEnabled()) {
        return;
    }
    (void)writeBoundsPng(dir + "/" + std::string(name) + ".png", title, curve);
}

void Visualizer::finalImage(const Graph &graph, const constraintMgr *fences, double hpwl) {
    if (!isEnabled()) {
        return;
    }
    flush();
    const double zoom = std::max(1.0, opts.finalZoom);
    ScopedTimer timer("final-image");
    const std::string path = dir + "/" + finalImageName;
    writeFinalFrameRaster(path, graph, fences, zoom, hpwl);
    const auto px = static_cast<int>(std::lround(zoom * 768.0));
    ktlog.echo("final placement image: {} ({}x{})", path, px, px);
}

bool Visualizer::finish(const Graph &graph, const std::vector<float> &x,
                        const std::vector<float> &y, const BBox &die, const constraintMgr *fences,
                        double hpwl) {
    flush();
    if (!animOn || framesKept < 2) {
        // One still is not an animation, and a two-frame GIF is a flicker; both
        // are more likely to be a mistake in the run than something worth
        // writing. Checked before the hold, so a run that recorded almost nothing
        // does not manufacture an animation out of its own hold frames.
        return false;
    }
    ScopedTimer timer("anim-finish");

    // Hold the finished placement before the GIF is written. Without it the
    // result -- the one picture anyone wants to look at -- is on screen for the
    // same time as any other single frame, and the animation scrolls past the
    // thing it spent minutes producing. Recorded rather than re-encoded, so it
    // costs one placement pass.
    const BBox &dieBox = die;
    for (std::size_t i = 0; i < opts.finalHold; ++i) {
        recordFrame(graph, x, y, dieBox, framesKept, 0, hpwl, 0.0, 0.0, finalNote, fences,
                    /*mandatory=*/true);
    }

    const bool written = writeAnimatedGif(animDir.string(), dir + "/" + animationName, delayCs);
    if (written) {
        std::error_code ec;
        std::filesystem::remove(animDir, ec);
    }
    if (!demandDir.empty() &&
        writeAnimatedGif(demandDir.string(), dir + "/" + demandAnimationName, delayCs)) {
        std::error_code ec;
        std::filesystem::remove(demandDir, ec);
    }
    return written;
}

/// Rasterise the placement as the next framesKept of the run. A no-op when the run
/// is not animOn, when the cap is reached, or when `x` and `y` are not one
/// entry per cell. The cap is checked before the work, not after, so a capped run
/// costs nothing.
void Visualizer::recordFrame(const Graph &g, const std::vector<float> &x,
                             const std::vector<float> &y, const BBox &die, std::size_t step,
                             std::size_t total, double hpwl, double hpwlInitial, double resid,
                             const std::string &note, const constraintMgr *constraints,
                             bool mandatory) {
    record(g, x, y, die, step, total, hpwl, hpwlInitial, resid, note, constraints, mandatory,
           false);
}

void Visualizer::record(const Graph &g, const std::vector<float> &x, const std::vector<float> &y,
                        const BBox &die, std::size_t step, std::size_t total, double hpwl,
                        double hpwlInitial, double resid, const std::string &note,
                        const constraintMgr *constraints, bool mandatory, bool inBackground) {
    const std::size_t limit = held < maxFrames ? maxFrames - held : maxFrames;
    // A mandatory frame is one per global-placement iteration, or the legalizer's
    // and the detail placer's own: sparse by construction. It is exempt from both
    // the stage budget and the thinning stride, because the dense per-CG-iteration
    // frames are what fill the budget and what gets thinned. Without the exemption
    // the whole LSS/LAL sequence was dropped from the GIF -- 12 frames out of
    // several hundred offered, so the stride skipped essentially all of them.
    if (!animOn || (!mandatory && framesKept >= limit)) {
        if (animOn && !cappedNow) {
            cappedNow = true;
            ktlog.echo(
                "animation: this stage used its {} frame allowance; later stages may not be "
                "represented. Raise 'visualizer anim_max_frames', or lower 'simpl cg_every'. "
                "The per-iteration SVGs are unaffected.",
                limit);
        }
        return;
    }
    // Every producer supplies one coordinate per vertex; a short vector would
    // index past the end inside the renderer.
    if (x.size() != g.getNumCells() || y.size() != g.getNumCells()) {
        return;
    }
    const auto emit = [&](const std::vector<float> &ix, const std::vector<float> &iy) {
        // Subsample rather than stop. Every CG iteration of every round is offered,
        // and once the byte budget is spent the spacing widens, so the animation
        // covers the whole run -- warm-up, LSS, LAL, legalization, detail placement
        // -- instead of exhausting itself in the first rounds and ending before
        // legalization begins.
        ++offered;
        // Mandatory frames bypass the stride. One framesKept per global-placement
        // iteration is only 12 of the several hundred offered once every
        // conjugate-gradient iteration is offered, so the subsampling that keeps
        // the warm-up affordable was dropping the entire LSS and LAL sequence --
        // the part of the run the animation most needs to show. Per-iteration
        // frames are the sparse ones by construction, so they are kept and the
        // dense ones are thinned instead.
        if (!mandatory && stride > 1 && ((offered - 1) % stride) != 0) {
            return true;
        }
        // Exempt for the same reason: the stage budget is reached long before the
        // warm-up's several hundred offered frames are, so without the exemption
        // the per-iteration frames still lost to the dense ones.
        if (framesKept >= limit && !mandatory) {
            stride *= 2;
            return true;
        }
        const std::string path = (animDir / getFrameName(framesKept)).string();
        // The caller's constraints, not nullptr: a run with fences drew them in
        // the per-iteration SVGs and then dropped them from the GIF, so the one
        // picture that gets looked at was missing the regions the placement was
        // required to respect.
        if (inBackground) {
            render([path, &g, ix, iy, die, hpwl, hpwlInitial, resid, note, constraints,
                    zoom = frameZoom, title = getHeading(note)] {
                writeFrameRaster(path, g, ix, iy, die, hpwl, hpwlInitial, resid, note, constraints,
                                 /*fixedView=*/true, zoom, /*drawRows=*/false, title);
            });
        } else {
            writeFrameRaster(path, g, ix, iy, die, hpwl, hpwlInitial, resid, note, constraints,
                             /*fixedView=*/true, frameZoom, /*drawRows=*/false, getHeading(note));
        }
        if (!demandDir.empty()) {
            const std::string demandPath = (demandDir / getFrameName(framesKept)).string();
            if (inBackground) {
                render([demandPath, &g, ix, iy, die, note, title = getHeading(note),
                        zoom = frameZoom, bins = opts.congestionBins] {
                    writeDemandRaster(demandPath, g, ix, iy, die, note, title, zoom, bins);
                });
            } else {
                writeDemandRaster(demandPath, g, ix, iy, die, note, getHeading(note), frameZoom,
                                  opts.congestionBins);
            }
        }
        ++framesKept;
        return framesKept < limit;
    };

    // The in-between frames first, so the animation arrives at this placement
    // rather than snapping to it.
    if (havePrev && blendSteps > 1 && prevX.size() == x.size()) {
        std::vector<float> bx(x.size()), by(y.size());
        for (int k = 1; k < blendSteps; ++k) {
            if (framesKept >= limit) {
                break;
            }
            const double t = static_cast<double>(k) / static_cast<double>(blendSteps);
            for (std::size_t v = 0; v < x.size(); ++v) {
                bx[v] = static_cast<float>(prevX[v] + t * (x[v] - prevX[v]));
                by[v] = static_cast<float>(prevY[v] + t * (y[v] - prevY[v]));
            }
            if (!emit(bx, by)) {
                break;
            }
        }
    }
    emit(x, y);

    prevX = x;
    prevY = y;
    havePrev = true;
}


namespace {

constexpr double margin = 36.0;  // image margin in pixels
// A band at the top reserved for the frame's text: title, wirelength, overflow and
// the colour key. The die is laid out below it rather than under it. The text used
// to be drawn straight onto the placement, so on any design whose cells reach the
// top of the die -- which is most of them, since the placer fills the core -- the
// first line of the caption sat on top of the cells and neither was readable.
// Sized to the caption block itself: the title sits at y=16, the wirelength at 36,
// the overflow at 56, and the colour key runs from 74 to about 105, so 116 leaves
// a row of clearance. Sized any tighter and the placement -- which is laid out
// bottom-anchored and so grows upward -- slides back under the last line of the
// key, which is how this was originally wrong.
constexpr double headerH = 116.0;
constexpr double imageW = 768.0;  // frame image size
constexpr double imageH = 768.0;

constexpr const char *textPrimary = "#e6edf3";
constexpr const char *textSecondary = "#9aa7b4";

std::string getGrouped(std::size_t n) {
    std::string digits = std::to_string(n);
    for (std::size_t at = digits.size(); at > 3; at -= 3) {
        digits.insert(at - 3, ",");
    }
    return digits;
}

/// Filename stem of the per-iteration stills that writeAnimatedGif() collects.
constexpr const char *framePrefix = "frame_";

// std::format rather than an ostringstream: a frame calls this four times per
// cell, and constructing a stream per number was most of the cost of writing one.
// "{:.Nf}" is the same correctly rounded fixed notation as std::fixed with
// setprecision(N), so the text is unchanged.
std::string getFmt(double v, int prec = 1) {
    return std::format("{:.{}f}", v, prec);
}

std::string getSci(double v) {
    std::ostringstream os;
    os << std::scientific << std::setprecision(1) << v;
    return os.str();
}

struct ViewPort {
    Point lowerLeft;
    double sx = 1.0, sy = 1.0;  // units -> px
    double offsetX = 0.0;
    double margin = 0.0;     // px, scaled by the frame's zoom
    double height = imageH;  // px, scaled by the frame's zoom
};

/// Scale that fits a spanX-by-spanY die into the drawing area: the full width less
/// the side margins, and the height less the header band and the bottom margin.
/// @p zoom multiplies the whole image geometry, so a frame rendered at zoom 4 is a
/// 3072x3072 picture of the same placement with four times the linear detail.
double fitScale(double spanX, double spanY, double zoom) {
    const double availW = zoom * imageW - 2.0 * zoom * margin - 2.0;
    const double availH = zoom * imageH - zoom * headerH - zoom * margin - 2.0;
    // The viewports pad minX/minY by 1% of the span, so the extent actually drawn
    // is 1.02 spans wide. Dividing by the padded span is what makes the die land
    // inside the area rather than 2% proud of it -- and since the die is anchored
    // to the bottom, "2% proud" is 2% *upward*, straight back under the caption.
    constexpr double pad = 1.02;
    return std::min(availW, availH) / (pad * std::max(std::max(spanX, spanY), 1.0));
}

ViewPort makeViewPort(const Graph &g, const std::vector<float> &x, const std::vector<float> &y,
                      const BBox &dieBox, double zoom) {
    const std::size_t nv = g.getNumCells();
    BBox box = BBox::getEmpty();
    for (std::size_t v = 0; v < nv; ++v) {
        box.extend(static_cast<double>(x[v]), static_cast<double>(y[v]));
    }
    box.extend(dieBox);
    const double spanX = std::max(box.getWidth(), 1.0);
    const double spanY = std::max(box.getHeight(), 1.0);
    const double sc = fitScale(spanX, spanY, zoom);
    ViewPort vp;
    vp.lowerLeft.x = box.lowerLeft.x - 0.01 * spanX;
    vp.lowerLeft.y = box.lowerLeft.y - 0.01 * spanY;
    vp.sx = sc;
    vp.sy = sc;
    vp.margin = zoom * margin;
    vp.height = zoom * imageH;
    return vp;
}

/// Viewport spanning exactly the die, so a series of frames shares one scale.
ViewPort getDieViewPort(const BBox &dieBox, double zoom) {
    ViewPort vp;
    const double spanX = std::max(dieBox.getWidth(), 1.0);
    const double spanY = std::max(dieBox.getHeight(), 1.0);
    const double sc = fitScale(spanX, spanY, zoom);
    vp.lowerLeft.x = dieBox.lowerLeft.x - 0.01 * spanX;
    vp.lowerLeft.y = dieBox.lowerLeft.y - 0.01 * spanY;
    vp.sx = sc;
    vp.sy = sc;
    vp.margin = zoom * margin;
    vp.height = zoom * imageH;
    return vp;
}

double toPxX(const ViewPort &vp, double v) {
    return vp.margin + vp.offsetX + (v - vp.lowerLeft.x) * vp.sx;
}
double toPxY(const ViewPort &vp, double v) {
    // Offset by the header so the die starts below the caption instead of under it.
    return vp.height - vp.margin - (v - vp.lowerLeft.y) * vp.sy;
}

// ---------------------------------------------------------------------------
// Raster (CImg) rendering
// ---------------------------------------------------------------------------

constexpr Rgb24 bgColor{.r = 0x10, .g = 0x14, .b = 0x18};

/// Parse "#rrggbb" as used throughout the SVG renderer.
Rgb24 getHexColor(const char *s) {
    Rgb24 c;
    unsigned v = 0;
    std::sscanf(s, "#%x", &v);
    c.r = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    c.g = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    c.b = static_cast<std::uint8_t>(v & 0xFF);
    return c;
}

/// Composite @p c over the flat background at @p alpha, returning a flat colour.
///
/// The SVG renderer relies on the SVG engine to do this. CImg can blend too, but
/// letting it blend on top of already-blended cells compounds opacity where cells
/// pile up and muddies the colours. Pre-blending against the background instead
/// keeps every drawn colour flat, which is what makes an exact GIF palette
/// possible in the first place.
/// A darker relative of @p c, for the rim that separates touching cells.
Rgb24 darken(const Rgb24 &c, double f) {
    return Rgb24{.r = static_cast<std::uint8_t>(std::lround(c.r * f)),
                 .g = static_cast<std::uint8_t>(std::lround(c.g * f)),
                 .b = static_cast<std::uint8_t>(std::lround(c.b * f))};
}

Rgb24 blendOnBg(const Rgb24 &c, double alpha) {
    // The three background channels differ, so blend each against its own value.
    auto chan = [alpha](std::uint8_t s, std::uint8_t bg) {
        return static_cast<std::uint8_t>(std::lround(alpha * s + (1.0 - alpha) * bg));
    };
    Rgb24 o;
    o.r = chan(c.r, bgColor.r);
    o.g = chan(c.g, bgColor.g);
    o.b = chan(c.b, bgColor.b);
    return o;
}

// The placement rows, as a y interval each, recovered from the cells.// A frame wants to draw the rows behind the cells -- it is the one thing in the// picture that says whether the placement is legal, because a cell that is on a// row and a cell that is not look identical otherwise -- and the renderer is// handed a graph and a set of coordinates, not a row list.// The rows are recovered from the cells instead, which works because a legal// placement is defined by it: every cell in a row has the same y. The distinct y// values are clustered with a tolerance of a fraction of the cell height, and// each cluster becomes one row. Before legalization there is no row structure to// recover -- the y values are continuous and the cluster count explodes -- so a// cluster count that is not plausible is reported as "no rows" and nothing is// drawn, rather than a frame covered in thousands of lines that mean nothing.

std::vector<std::array<double, 2>> getRowBands(const Graph &g, const std::vector<float> &y) {
    std::vector<double> ys;
    std::vector<double> heights;
    const std::size_t nv = g.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        if (!vert.isFixed && !vert.isTerminal) {
            ys.push_back(static_cast<double>(y[v]));
            heights.push_back(vert.height);
        }
    }
    if (ys.empty()) {
        return {};
    }
    std::sort(ys.begin(), ys.end());
    // Two y values are the same row if they are within a small fraction of a row
    // height. Large enough to absorb the float rounding in a coordinate that has
    // been through a solve, small enough that neighbouring rows never merge.
    //
    // The tolerance is scaled by the *median* cell height, not the largest. The
    // largest is the wrong number by a wide margin: a design with a handful of
    // multi-row cells has a maximum several rows tall, and a tolerance taken from
    // it swallows that many rows at once, so the whole placement collapses into
    // one band and no rows are found at all.
    std::sort(heights.begin(), heights.end());
    const double tol = std::max(heights[heights.size() / 2], 1e-9) * 0.25;
    std::vector<std::array<double, 2>> bands;
    double lo = ys.front();
    double hi = ys.front();
    for (std::size_t i = 1; i < ys.size(); ++i) {
        if (ys[i] - hi <= tol) {
            hi = ys[i];
            continue;
        }
        bands.push_back({lo, hi});
        lo = hi = ys[i];
    }
    bands.push_back({lo, hi});
    // The rows have to be evenly spaced, and that is the check that makes this
    // safe to run on every frame rather than only the last one. A legalized
    // placement puts its cells on a row grid, so consecutive bands are a whole
    // number of row pitches apart. Before legalization the y values are whatever
    // the solve produced: they still form clusters, because a solution tends to
    // settle onto levels, and a count alone cannot tell those apart from real
    // rows. Their spacing, though, is irregular, so they are rejected and the
    // frame gets no row lines rather than several hundred meaningless ones.
    if (bands.size() < 2 || bands.size() > 8192) {
        return {};
    }
    // Median gap, which a couple of odd rows cannot move.
    std::vector<double> gaps;
    gaps.reserve(bands.size() - 1);
    for (std::size_t i = 1; i < bands.size(); ++i) {
        gaps.push_back(bands[i][0] - bands[i - 1][0]);
    }
    std::sort(gaps.begin(), gaps.end());
    const double pitch = gaps[gaps.size() / 2];
    if (pitch <= tol) {
        return {};
    }
    // Every gap a whole number of pitches, to within a tenth of one. A legal row
    // grid has no gap that is not; an empty row is two pitches and is fine, but a
    // cluster decomposition of an unlegalized placement is not a multiple of
    // anything.
    for (const double gap : gaps) {
        const double k = std::round(gap / pitch);
        if (k < 1.0 || std::fabs(gap - k * pitch) > 0.1 * pitch) {
            return {};
        }
    }
    return bands;
}

// Shared colour table for a whole animation.// GIF stores palette indices, so an animation is only correct if every frame// indexes the same table. Indices are therefore handed out on first use and the// table is kept alive across all frames; the renderer asks for a colour by value// and always gets the same index back.

/// Pack a colour into a map key.
std::uint32_t packRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return (static_cast<std::uint32_t>(r) << 16) | (static_cast<std::uint32_t>(g) << 8) | b;
}

/// Median-cut quantisation of a colour histogram down to at most @p maxColors.
///
/// Splits the colour box on its longest axis at the median of the weighted
/// distribution, repeatedly, until the budget is spent. Weighting by how often a
/// colour occurs is what keeps the result faithful: a background that covers half
/// the frame gets half the palette, and a one-off antialiased edge shade that
/// appears on four pixels does not.
std::vector<Rgb24> getMedianCutPalette(const std::map<std::uint32_t, std::uint32_t> &hist,
                                       std::size_t maxColors) {
    std::vector<Rgb24> out;
    if (hist.empty()) {
        return out;
    }
    struct Box {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;  // key, count
    };
    std::vector<Box> boxes(1);
    for (const auto &[key, count] : hist) {
        boxes[0].entries.emplace_back(key, count);
    }
    const auto average = [](const Box &box) {
        double r = 0, g = 0, b = 0, w = 0;
        for (const auto &[key, count] : box.entries) {
            const double ww = count;
            r += ww * static_cast<double>((key >> 16) & 0xff);
            g += ww * static_cast<double>((key >> 8) & 0xff);
            b += ww * static_cast<double>(key & 0xff);
            w += ww;
        }
        if (w <= 0.0) {
            return Rgb24{.r = 0, .g = 0, .b = 0};
        }
        return Rgb24{.r = static_cast<std::uint8_t>(std::lround(r / w)),
                     .g = static_cast<std::uint8_t>(std::lround(g / w)),
                     .b = static_cast<std::uint8_t>(std::lround(b / w))};
    };

    while (boxes.size() < maxColors) {
        // Split the box with the largest weighted spread, which is the one whose
        // average is furthest from representing its contents.
        std::size_t bestIdx = boxes.size();
        int bestAxis = 0;
        double bestScore = 0.0;
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            if (boxes[i].entries.size() < 2) {
                continue;
            }
            std::uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
            for (const auto &[key, count] : boxes[i].entries) {
                (void)count;
                const std::uint8_t c[3] = {static_cast<std::uint8_t>((key >> 16) & 0xff),
                                           static_cast<std::uint8_t>((key >> 8) & 0xff),
                                           static_cast<std::uint8_t>(key & 0xff)};
                for (int k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], c[k]);
                    hi[k] = std::max(hi[k], c[k]);
                }
            }
            int axis = 0;
            int span = hi[0] - lo[0];
            for (int k = 1; k < 3; ++k) {
                if (hi[k] - lo[k] > span) {
                    span = hi[k] - lo[k];
                    axis = k;
                }
            }
            if (span <= 0) {
                continue;
            }
            const double score =
                static_cast<double>(span) * static_cast<double>(boxes[i].entries.size());
            if (score > bestScore) {
                bestScore = score;
                bestIdx = i;
                bestAxis = axis;
            }
        }
        if (bestIdx == boxes.size()) {
            break;  // nothing left worth splitting
        }
        Box &box = boxes[bestIdx];
        const int shift = (bestAxis == 0) ? 16 : (bestAxis == 1 ? 8 : 0);
        std::stable_sort(box.entries.begin(), box.entries.end(),
                         [shift](const auto &a, const auto &b) {
                             return ((a.first >> shift) & 0xff) < ((b.first >> shift) & 0xff);
                         });
        const std::size_t mid = box.entries.size() / 2;
        Box lo, hi;
        lo.entries.assign(box.entries.begin(), box.entries.begin() + mid);
        hi.entries.assign(box.entries.begin() + mid, box.entries.end());
        boxes[bestIdx] = std::move(lo);
        boxes.push_back(std::move(hi));
    }
    out.reserve(boxes.size());
    for (const Box &box : boxes) {
        if (!box.entries.empty()) {
            out.push_back(average(box));
        }
    }
    return out;
}

class GifPalette {
public:
    /// Register @p c if new and return its index. Falls back to the closest
    /// existing entry once the table is full (256 colours, GIF's hard limit).
    std::uint8_t getIndex(const Rgb24 &c) {
        const std::uint32_t key = pack(c);
        auto it = cache.find(key);
        if (it != cache.end()) {
            return it->second;
        }
        if (colorTable.size() < 256) {
            const auto idx = static_cast<std::uint8_t>(colorTable.size());
            colorTable.push_back(c);
            cache.emplace(key, idx);
            return idx;
        }
        return getNearest(c);
    }

    const std::vector<Rgb24> &getColors() const {
        return colorTable;
    }

    /// CImg colour pointer (cimg::spectrum() consecutive values) for drawing.
    /// The slot is (re)written on every call, which is idempotent because
    /// index() is stable for a given colour.
    const std::uint8_t *getPtr(const Rgb24 &c) {
        const std::size_t i = getIndex(c);
        lut[3 * i + 0] = c.r;
        lut[3 * i + 1] = c.g;
        lut[3 * i + 2] = c.b;
        return lut.data() + 3 * i;
    }

private:
    static std::uint32_t pack(const Rgb24 &c) {
        return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) |
               static_cast<std::uint32_t>(c.b);
    }

    std::uint8_t getNearest(const Rgb24 &c) const {
        std::size_t best = 0;
        long bestD = std::numeric_limits<long>::max();
        for (std::size_t i = 0; i < colorTable.size(); ++i) {
            const long dr = colorTable[i].r - c.r, dg = colorTable[i].g - c.g,
                       db = colorTable[i].b - c.b;
            const long d = dr * dr + dg * dg + db * db;
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return static_cast<std::uint8_t>(best);
    }

    std::vector<Rgb24> colorTable;
    std::vector<std::uint8_t> lut = std::vector<std::uint8_t>(256 * 3, 0);
    std::unordered_map<std::uint32_t, std::uint8_t> cache;
};

/// Rounded, ordered pixel span for a filled rectangle (CImg needs ints).
void pixelSpan(double a, double b, int &lo, int &hi) {
    lo = static_cast<int>(std::lround(a));
    hi = static_cast<int>(std::lround(b));
    if (hi < lo) {
        std::swap(lo, hi);
    }
    if (hi == lo) {
        ++hi;  // keep degenerate cells visible
    }
}

/// Advance width of @p s in @p font, mirroring CImg's own text layout.
///
/// CImg's draw_text() returns the image, not the drawn width, so a legend that
/// has to place the next swatch after a label has to measure the text itself.
int getTextWidth(const CImgList<unsigned char> &font, const char *s) {
    if (font._width <= 0) {
        return 0;
    }
    const int h = font[0]._height;
    const int w = static_cast<int>(font._width);
    const int pad = h < 48 ? 1 : (h < 128 ? static_cast<int>(std::ceil(h / 51.0f + 0.745f)) : 4);
    int total = 0;
    for (const char *p = s; *p; ++p) {
        const int ch = static_cast<unsigned char>(*p);
        if (ch == ' ') {
            total += w > 32 ? font[32]._width : font[0]._width;
        } else if (ch < w) {
            total += font[ch]._width + pad;
        }
    }
    return total;
}

// Raster counterpart of writeFrameSvg(), drawn with CImg.// Mirrors the SVG renderer's layout, colours and draw order so the two// representations of a frame agree. Everything is drawn flat (see blendOnBg)// so the result maps onto the GIF palette without any colour reduction.

namespace {
// Analytic anti-aliased fill of an axis-aligned rectangle, in fractional pixel
// coordinates, blended over what is already there.
//
// CImg's draw_rectangle snaps to whole pixels, which is what makes a placement
// frame look like a grid of hard blocks: a standard cell is a few pixels across,
// so every edge lands somewhere arbitrary and neighbouring cells either touch or
// leave a hard one-pixel seam. At that scale the staircase *is* the picture, and it
// makes the eye read a coarse mosaic rather than a placement. Averaging coverage
// per pixel is what a graphics API would do, and it costs a multiply per pixel
// rather than a supersampled render of the whole frame.
//
// alpha is the fill's own opacity; the colour has already been flattened onto the
// background by the caller, so this is a straight lerp between the existing pixel
// and the fill.
void fillRectAA(CImg<unsigned char> &img, double x0f, double y0f, double x1f, double y1f,
                const std::uint8_t *rgb, double alpha) {
    if (x1f <= x0f || y1f <= y0f) {
        return;
    }
    // The rectangle is in FRACTIONAL pixel coordinates. Rounding it to whole
    // pixels before getting here -- which is what the old draw_rectangle path did
    // -- makes every pixel's coverage exactly 1, and the antialiasing silently
    // does nothing while still looking like it compiled and ran.
    const int px0 = std::max(0, static_cast<int>(std::floor(x0f)));
    const int py0 = std::max(0, static_cast<int>(std::floor(y0f)));
    const int px1 = std::min(img.width() - 1, static_cast<int>(std::ceil(x1f)));
    const int py1 = std::min(img.height() - 1, static_cast<int>(std::ceil(y1f)));
    for (int py = py0; py <= py1; ++py) {
        const double cy = std::min<double>(y1f, py + 1) - std::max<double>(y0f, py);
        if (cy <= 0.0) {
            continue;
        }
        for (int px = px0; px <= px1; ++px) {
            const double cx = std::min<double>(x1f, px + 1) - std::max<double>(x0f, px);
            if (cx <= 0.0) {
                continue;
            }
            // Posterise the coverage to a handful of levels.
            //
            // A GIF has one 256-entry palette for the whole animation, and the
            // antialiased edges of a 200k-cell placement produce well over a
            // thousand distinct shades on their own. Once the shades outnumber the
            // slots, the quantiser cannot give two frames the same index for the
            // same colour, so a cell that has not moved changes colour between
            // frames -- the one artefact that makes a placement animation look
            // broken rather than merely rough. Six levels is indistinguishable
            // from continuous coverage at this scale and costs a twentieth of the
            // palette. The floor keeps a barely-touched pixel visible instead of
            // rounding it away to nothing.
            constexpr double aaLevels = 6.0;
            double a = alpha * cx * cy;
            a = std::round(a * aaLevels) / aaLevels;
            if (a <= 0.0) {
                continue;
            }
            a = std::max(a, 1.0 / aaLevels);
            a = std::min(a, 1.0);
            for (int ch = 0; ch < 3; ++ch) {
                const double dst = img(px, py, 0, ch);
                img(px, py, 0, ch) = static_cast<std::uint8_t>(
                    std::lround(dst + a * (static_cast<double>(rgb[ch]) - dst)));
            }
        }
    }
}
// The same, for a rectangle's *outline* rather than its interior. Four filled
// bands rather than one fillRectAA, because filling the box would paint the die
// solid: the die frame is a wall, not a surface.
void outlineRect(CImg<unsigned char> &img, int x0, int y0, int x1, int y1,
                 const std::uint8_t *rgb) {
    img.draw_rectangle(x0, y0, x1, y0, rgb);
    img.draw_rectangle(x0, y1, x1, y1, rgb);
    img.draw_rectangle(x0, y0, x0, y1, rgb);
    img.draw_rectangle(x1, y0, x1, y1, rgb);
}

void strokeRectAA(CImg<unsigned char> &img, double x0, double y0, double x1, double y1, double t,
                  const std::uint8_t *rgb, double alpha) {
    if (t <= 0.0 || x1 <= x0 || y1 <= y0) {
        return;
    }
    fillRectAA(img, x0, y0, x1, y0 + t, rgb, alpha);
    fillRectAA(img, x0, y1 - t, x1, y1, rgb, alpha);
    fillRectAA(img, x0, y0 + t, x0 + t, y1 - t, rgb, alpha);
    fillRectAA(img, x1 - t, y0 + t, x1, y1 - t, rgb, alpha);
}
// Draw a cell-sized rectangle, antialiased only when it is big enough for the
// antialiasing to be an improvement rather than a liability.
//
// A standard cell in a 200k-cell design is two or three pixels across at 768px.
// Antialiasing something that small is counterproductive: nearly every pixel is
// an edge pixel, so the cell's apparent colour becomes a function of its
// sub-pixel position, and it visibly changes shade as it drifts across the grid
// -- the placement shimmers even when nothing is happening. That is also why the
// GIF does not match the SVG, where a cell is one flat fill.
//
// Past a few pixels the arithmetic reverses: long straight edges stop looking
// like staircases, and the coverage-weighted edge is what makes them look drawn
// rather than pixelated. So the threshold is where the two effects cross, and
// below it the cell is snapped to whole pixels exactly as the SVG draws it.
void fillCellRect(CImg<unsigned char> &img, double x0, double yTop, double x1, double yBot,
                  const std::uint8_t *rgb, const std::uint8_t *rim) {
    // Always antialiased, at every size.
    //
    // The obvious optimisation is to snap a small cell to whole pixels -- it is
    // cheaper and, at two or three pixels across, the antialiasing is arguably
    // counterproductive. It is also exactly what makes a legal placement look
    // like an overlapping one. Snapping rounds each edge, so every cell grows or
    // shrinks by up to half a pixel on each side; the errors do not cancel, so
    // neighbouring cells overlap in the drawing by up to a pixel while the
    // placement they are drawn from has them exactly touching. A run of 200k such
    // cells fills in the gaps between them and the placement reads as a solid
    // field, which is a picture of overlapping cells and nothing like the
    // placement. Drawing the true fractional edges is the only way the picture
    // can show the cells the same size and position as the ones being written out.
    fillRectAA(img, x0, yTop, x1, yBot, rgb, 1.0);
    // A darker rim, inset so it lies inside the cell rather than eating into its
    // neighbour.
    //
    // Standard cells in a dense placement abut each other exactly -- that is what
    // legal placement means -- so with one flat fill the eye cannot find where one
    // cell stops and the next begins, and a whole region reads as a single blob.
    // The rim is what makes a packed placement legible: it costs one outline per
    // cell and it is the difference between "a picture of a placement" and "a
    // picture of cells".
    //
    // Its width is capped hard, and this is the difference between a picture of a
    // placement and a picture of boxes. A legal standard cell is one row tall, so
    // at any useful resolution it is only a handful of pixels tall, and a rim
    // sized as a fixed fraction of the cell -- or, worse, a full pixel -- eats a
    // large part of it: every cell becomes a dark frame around a thin bright core
    // and a whole row of them reads as a stack of horizontal bars rather than as
    // cells. So the rim is at most a hairline, and it is skipped entirely on a
    // cell too small to carry one, where the rim would be most of the cell.
    constexpr double maxRim = 0.55;  // pixels
    const double w = std::min({maxRim, (x1 - x0) * 0.18, (yBot - yTop) * 0.18});
    if (w < 0.2) {
        return;  // a cell too small to outline without hollowing it out
    }
    strokeRectAA(img, x0 + w / 2, yTop + w / 2, x1 - w / 2, yBot - w / 2, w, rim, 1.0);
}
}  // namespace

CImg<unsigned char> renderFrameCImg(const Graph &g, const std::vector<float> &x,
                                    const std::vector<float> &y, const BBox &dieBox, double hpwl,
                                    double hpwlInitial, double resid, const std::string &note,
                                    const constraintMgr *constraints, bool fixedView,
                                    GifPalette &pal, double zoom, bool drawRows,
                                    const std::string &heading) {
    zoom = std::max(zoom, 1.0);
    const std::size_t nv = g.getNumCells();
    ViewPort vp = fixedView ? getDieViewPort(dieBox, zoom) : makeViewPort(g, x, y, dieBox, zoom);
    if (fixedView) {
        const double drawn = 1.02 * std::max(dieBox.getWidth(), 1.0) * vp.sx;
        vp.offsetX = std::max(0.0, 0.5 * (zoom * imageW - 2.0 * vp.margin - drawn));
    }

    const int imgW = static_cast<int>(std::lround(zoom * imageW));
    const int imgH = static_cast<int>(std::lround(zoom * imageH));
    CImg<unsigned char> img(imgW, imgH, 1, 3);
    // Fill the background channel by channel. CImg's fill(values, true) cannot be
    // used here: its repeat loop never advances the source pointer, so everything
    // past the first pixel would repeat the red channel and the image would come
    // out grey (r,r,r) instead of the intended background colour.
    const std::array<std::uint8_t, 3> bgv = bgColor.getRgb();
    cimg_forXYC(img, px, py, ch) {
        img(px, py, 0, ch) = bgv[ch];
    }

    // No progress bar. It sat along the bottom of every frame, which is exactly
    // where the placement is: on a die whose cells reach the bottom rows the bar
    // covered them, and an animation is judged on the placement, not on how far
    // along it is. The iteration number is already in the frame's note text, and
    // the per-iteration CSVs carry the same series in numbers.

    // Die (fixed-pad) frame. Drawn as a band rather than a hairline: at 768px
    // across a 11000um die one unit is under a tenth of a pixel, so a
    // single-pixel outline of a light grey all but disappears against the dark
    // background and leaves the viewer with no fixed reference for where the
    // placement is allowed to be. A dark outer edge against a light inner one
    // keeps the boundary legible at any zoom and against cells of either colour.
    {
        int x0, y0, x1, y1;
        pixelSpan(toPxX(vp, dieBox.lowerLeft.x), toPxX(vp, dieBox.upperRight.x), x0, x1);
        pixelSpan(toPxY(vp, dieBox.upperRight.y), toPxY(vp, dieBox.lowerLeft.y), y0, y1);
        // A pad the frame is drawn inside, so the band is a band rather than a
        // stroke over the outermost row of cells.
        const int inset = std::max(1, static_cast<int>(std::lround(2.0 * zoom)));
        const int gx0 = x0 + inset, gy0 = y0 + inset;
        const int gx1 = x1 - inset, gy1 = y1 - inset;
        // Outside: a black keyline, so the boundary separates from anything
        // behind it. Inside: near-white, so it reads as a wall. Both antialiased,
        // because a border is exactly the thing the eye uses to judge whether the
        // placement inside it is aligned, and a staircase there is very visible.
        strokeRectAA(img, x0 - 1, y0 - 1, x1 + 1, y1 + 1, 1.0, pal.getPtr(getHexColor("#000000")),
                     1.0);
        strokeRectAA(img, gx0, gy0, gx1, gy1, std::max(1.0, 1.25 * zoom),
                     pal.getPtr(getHexColor("#8b97a3")), 1.0);
    }

    // Fence regions, under the cells so the placement stays readable.
    if (constraints != nullptr) {
        static const char *fenceColors[] = {"#ffb74d", "#ba68c8", "#4db6ac", "#f06292",
                                            "#9575cd", "#ffd54f", "#4fc3f7", "#a1887f"};
        for (std::size_t ri = 0; ri < constraints->getNumRegions(); ++ri) {
            const Region &reg = *constraints->getRegion(static_cast<int>(ri));
            const char *hex = fenceColors[ri % (sizeof(fenceColors) / sizeof(char *))];
            const Rgb24 flat = blendOnBg(getHexColor(hex), 0.13);
            const Rgb24 edge = blendOnBg(getHexColor(hex), 0.9);
            for (const BBox &r : reg.rects) {
                int x0, y0, x1, y1;
                pixelSpan(toPxX(vp, r.lowerLeft.x), toPxX(vp, r.upperRight.x), x0, x1);
                pixelSpan(toPxY(vp, r.upperRight.y), toPxY(vp, r.lowerLeft.y), y0, y1);
                img.draw_rectangle(x0, y0, x1, y1, pal.getPtr(flat), 1.0f);
                outlineRect(img, x0, y0, x1, y1, pal.getPtr(edge));
            }
            img.draw_text(
                static_cast<int>(toPxX(vp, reg.bounds.lowerLeft.x)) + static_cast<int>(3 * zoom),
                static_cast<int>(toPxY(vp, reg.bounds.lowerLeft.y)) - static_cast<int>(3 * zoom),
                "%s", pal.getPtr(edge), 0, 1.0f,
                &CImgList<unsigned char>::font(static_cast<unsigned int>(std::lround(13.0 * zoom))),
                reg.name.c_str());
        }
    }

    const CImgList<unsigned char> &font =
        CImgList<unsigned char>::font(static_cast<unsigned int>(std::lround(13.0 * zoom)));

    const std::vector<std::array<double, 2>> rows =
        drawRows ? getRowBands(g, y) : std::vector<std::array<double, 2>>{};

    // Row lines, at the very back. Faint, and only every eighth row, which turns
    // them from a hatch into a ruler: a line at every row pitch on a design with a
    // thousand rows is a line every five pixels, the whole placement reads as
    // stripes, and the cells disappear into the ruling. Every eighth still shows
    // the row structure and the pitch, and leaves the picture alone.
    if (!rows.empty()) {
        const Rgb24 line = blendOnBg(getHexColor("#8fa3b8"), 0.34);
        int rx0, rx1, ry0, ry1;
        pixelSpan(toPxX(vp, dieBox.lowerLeft.x), toPxX(vp, dieBox.upperRight.x), rx0, rx1);
        constexpr std::size_t every = 8;
        for (std::size_t i = 0; i < rows.size(); i += every) {
            // The top edge of the row is the line that matters: a cell sitting in
            // this row has its bottom exactly there.
            pixelSpan(toPxY(vp, rows[i][1]), toPxY(vp, rows[i][1]), ry0, ry1);
            fillRectAA(img, rx0, ry0, rx1, std::max(ry0 + 1, ry1), pal.getPtr(line), 1.0);
        }
    }

    // Draw order is macros first and movable cells over them, which is the reverse
    // of the obvious. It matters only where the two overlap, and they overlap
    // exactly where there is something to see: before legalization the placer has
    // no reason to keep a cell out of a macro, so a global-placement frame is full
    // of cells sitting on top of the macros. Painting the macros last buried every
    // one of them, and the movable cells -- the thing the frame is about --
    // disappeared under the fixed geometry. The macros stay legible: they are the
    // larger shape, and a cell drawn over one shows the macro's extent by the part
    // of it that nothing covers.
    {
        const Rgb24 flat = blendOnBg(getHexColor("#ef5350"), 0.9);
        const std::uint8_t *c = pal.getPtr(flat);
        const std::uint8_t *rim = pal.getPtr(darken(flat, 0.72));
        const auto drawFixed = [&](std::size_t v) {
            const Vertex &vert = g.getCell(v);
            const double x0 = toPxX(vp, x[v]);
            const double x1 = x0 + std::max(2.0, vert.width * vp.sx);
            const double yTop = toPxY(vp, y[v] + vert.height);
            const double yBot = toPxY(vp, y[v]);
            fillCellRect(img, x0, yTop, x1, yBot, c, rim);
        };
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (vert.isFixed) {
                drawFixed(v);
            }
        }
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (vert.isTerminal && !vert.isFixed) {
                drawFixed(v);
            }
        }
    }

    // Movable cells in one flat blue with a dark rim, over everything else. The rim
    // is the part that makes a legal placement readable: a legal placement is a
    // field of cells abutting exactly, so with one flat fill the eye cannot find
    // where one stops and the next begins and a whole region reads as a single
    // blob. The rim costs one outline per cell and it is the difference between "a
    // picture of a placement" and "a picture of cells".
    {
        const Rgb24 flat = blendOnBg(getHexColor("#4fc3f7"), 0.92);
        const std::uint8_t *c = pal.getPtr(flat);
        const std::uint8_t *rim = pal.getPtr(darken(flat, 0.72));
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (vert.isFixed || vert.isTerminal) {
                continue;
            }
            // Fractional edges, so the cell covers exactly the pixels it covers in
            // the placement. Rounding each edge to a whole pixel -- or widening a
            // sub-pixel cell to a minimum of one -- is what makes a picture of a
            // legal placement look like an overlapping one: every cell grows by up
            // to a pixel and 200k of them no longer fit side by side. At this
            // resolution a standard cell is several pixels across, so the exact
            // size is both available and what the eye needs. The 0.75 floor only
            // guards a cell narrower than a pixel, which would otherwise vanish.
            // toPxY is inverted -- world +y is pixel -y -- so the cell's TOP edge
            // is the smaller pixel row. Getting that backwards makes every cell
            // an empty box and the frame comes out blank.
            const double x0 = toPxX(vp, x[v]);
            const double x1 = x0 + std::max(0.75, vert.width * vp.sx);
            const double yTop = toPxY(vp, y[v] + vert.height);
            const double yBot = toPxY(vp, y[v]);
            fillCellRect(img, x0, yTop, x1, yBot, c, rim);
        }
    }

    const auto px = [zoom](double v) {
        return static_cast<int>(std::lround(v * zoom));
    };
    const auto ink = [&pal](const char *hex) {
        return pal.getPtr(getHexColor(hex));
    };
    const CImgList<unsigned char> &titleFont =
        CImgList<unsigned char>::font(static_cast<unsigned int>(std::lround(19.0 * zoom)));
    const int left = px(margin);
    const int right = px(imageW - margin);

    img.draw_text(left, px(14), "%s", ink(textPrimary), 0, 1.0f, &titleFont, heading.c_str());
    img.draw_text(left, px(40), "%s", ink(textSecondary), 0, 1.0f, &font, note.c_str());

    if (hpwl > 0.0) {
        const std::string wl = std::format("HPWL {:.4e}", hpwl);
        img.draw_text(right - getTextWidth(titleFont, wl.c_str()), px(14), "%s", ink(textPrimary),
                      0, 1.0f, &titleFont, wl.c_str());
        std::string detail;
        if (hpwlInitial > 0.0) {
            const double change = 100.0 * (hpwl - hpwlInitial) / hpwlInitial;
            detail = std::format("{:+.2f}% vs stage input", change);
        }
        if (resid > 0.0) {
            detail += std::format("{}overflow {:.1f}%", detail.empty() ? "" : "   ", 100.0 * resid);
        }
        if (!detail.empty()) {
            img.draw_text(right - getTextWidth(font, detail.c_str()), px(40), "%s",
                          ink(textSecondary), 0, 1.0f, &font, detail.c_str());
        }
    }

    {
        std::size_t movable = 0, fixed = 0, pads = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (vert.isFixed) {
                ++fixed;
            } else if (vert.isTerminal) {
                ++pads;
            } else {
                ++movable;
            }
        }
        const int legendY = px(68);
        const int swatch = px(10);
        const int swatchY = legendY + (font[0]._height - swatch) / 2;
        int cx = left;
        const auto entry = [&](Rgb24 colour, const std::string &label) {
            img.draw_rectangle(cx, swatchY, cx + swatch, swatchY + swatch, pal.getPtr(colour),
                               1.0f);
            cx += swatch + px(7);
            img.draw_text(cx, legendY, "%s", ink(textSecondary), 0, 1.0f, &font, label.c_str());
            cx += getTextWidth(font, label.c_str()) + px(22);
        };
        entry(blendOnBg(getHexColor("#4fc3f7"), 0.92), "Movable cells " + getGrouped(movable));
        if (fixed > 0) {
            entry(blendOnBg(getHexColor("#ef5350"), 0.9), "Fixed macros " + getGrouped(fixed));
        }
        if (pads > 0) {
            entry(blendOnBg(getHexColor("#ef5350"), 0.9), "I/O pads " + getGrouped(pads));
        }
        if (!rows.empty()) {
            entry(blendOnBg(getHexColor("#5c6b7a"), 0.9), "Rows " + getGrouped(rows.size()));
        }
    }

    return img;
}

namespace {

std::shared_ptr<const NetlistPins> getPinsOf(const Graph &g) {
    static std::mutex guard;
    static const Graph *cachedGraph = nullptr;
    static std::size_t cachedPins = 0;
    static std::shared_ptr<const NetlistPins> cached;
    const std::lock_guard lock(guard);
    if (cachedGraph != &g || cachedPins != g.getNumPins() || !cached) {
        cached = std::make_shared<const NetlistPins>(NetlistPins::of(g));
        cachedGraph = &g;
        cachedPins = g.getNumPins();
    }
    return cached;
}

CImg<unsigned char> renderDemandCImg(const Graph &g, const std::vector<float> &x,
                                     const std::vector<float> &y, const BBox &dieBox,
                                     const std::string &note, const std::string &heading,
                                     GifPalette &pal, double zoom, std::size_t bins) {
    zoom = std::max(zoom, 1.0);
    ViewPort vp = getDieViewPort(dieBox, zoom);
    const double drawn = 1.02 * std::max(dieBox.getWidth(), 1.0) * vp.sx;
    vp.offsetX = std::max(0.0, 0.5 * (zoom * imageW - 2.0 * vp.margin - drawn));
    const int imgW = static_cast<int>(std::lround(zoom * imageW));
    const int imgH = static_cast<int>(std::lround(zoom * imageH));
    CImg<unsigned char> img(imgW, imgH, 1, 3);
    const std::array<std::uint8_t, 3> bgv = bgColor.getRgb();
    cimg_forXYC(img, px, py, ch) {
        img(px, py, 0, ch) = bgv[ch];
    }

    const std::shared_ptr<const NetlistPins> pins = getPinsOf(g);
    const RoutingDemand map = getRoutingDemand(pins->netPinStart, pins->pinCell, pins->pinDx,
                                               pins->pinDy, x, y, dieBox, bins);
    const float top = map.p99 > 0.0F ? map.p99 : std::max(map.peak, 1e-9F);
    const auto heat = [&pal](float scaled) {
        const std::array<std::uint8_t, 4> c = getDemandColour(scaled);
        return std::make_pair(
            pal.getPtr(blendOnBg(Rgb24{.r = c[0], .g = c[1], .b = c[2]}, c[3] / 255.0)), c[3] > 0);
    };
    for (std::size_t iy = 0; iy < map.ny; ++iy) {
        const double y0 = map.origin.y + static_cast<double>(iy) * map.binH;
        for (std::size_t ix = 0; ix < map.nx; ++ix) {
            const auto [colour, visible] = heat(map.at(ix, iy) / top);
            if (!visible) {
                continue;
            }
            const double x0 = map.origin.x + static_cast<double>(ix) * map.binW;
            fillRectAA(img, toPxX(vp, x0), toPxY(vp, y0 + map.binH), toPxX(vp, x0 + map.binW),
                       toPxY(vp, y0), colour, 1.0);
        }
    }

    const std::uint8_t *macroEdge = pal.getPtr(blendOnBg(getHexColor("#ef5350"), 0.6));
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &cell = g.getCell(v);
        if (!cell.isFixed) {
            continue;
        }
        strokeRectAA(img, toPxX(vp, x[v]), toPxY(vp, y[v] + cell.height),
                     toPxX(vp, x[v] + cell.width), toPxY(vp, y[v]), std::max(1.0, 0.6 * zoom),
                     macroEdge, 1.0);
    }
    {
        int x0, y0, x1, y1;
        pixelSpan(toPxX(vp, dieBox.lowerLeft.x), toPxX(vp, dieBox.upperRight.x), x0, x1);
        pixelSpan(toPxY(vp, dieBox.upperRight.y), toPxY(vp, dieBox.lowerLeft.y), y0, y1);
        const int inset = std::max(1, static_cast<int>(std::lround(2.0 * zoom)));
        strokeRectAA(img, x0 - 1, y0 - 1, x1 + 1, y1 + 1, 1.0, pal.getPtr(getHexColor("#000000")),
                     1.0);
        strokeRectAA(img, x0 + inset, y0 + inset, x1 - inset, y1 - inset,
                     std::max(1.0, 1.25 * zoom), pal.getPtr(getHexColor("#8b97a3")), 1.0);
    }

    const auto px = [zoom](double v) {
        return static_cast<int>(std::lround(v * zoom));
    };
    const auto ink = [&pal](const char *hex) {
        return pal.getPtr(getHexColor(hex));
    };
    const CImgList<unsigned char> &font =
        CImgList<unsigned char>::font(static_cast<unsigned int>(std::lround(13.0 * zoom)));
    const CImgList<unsigned char> &titleFont =
        CImgList<unsigned char>::font(static_cast<unsigned int>(std::lround(19.0 * zoom)));
    const int left = px(margin);
    const int right = px(imageW - margin);
    img.draw_text(left, px(14), "%s", ink(textPrimary), 0, 1.0f, &titleFont,
                  ("Routing demand - " + heading).c_str());
    img.draw_text(left, px(40), "%s", ink(textSecondary), 0, 1.0f, &font, note.c_str());
    const std::string peak = std::format("peak {:.2f} nets/bin", map.peak);
    img.draw_text(right - getTextWidth(titleFont, peak.c_str()), px(14), "%s", ink(textPrimary), 0,
                  1.0f, &titleFont, peak.c_str());
    const std::string spread = std::format("99th pct {:.2f}   mean {:.2f}   grid {}x{}", map.p99,
                                           map.mean, map.nx, map.ny);
    img.draw_text(right - getTextWidth(font, spread.c_str()), px(40), "%s", ink(textSecondary), 0,
                  1.0f, &font, spread.c_str());

    const std::string label = "nets per bin: 0";
    const int legendY = px(68);
    img.draw_text(left, legendY, "%s", ink(textSecondary), 0, 1.0f, &font, label.c_str());
    const int barLeft = left + getTextWidth(font, label.c_str()) + px(8);
    const int barWidth = px(220);
    const int barTop = legendY + px(2);
    for (int i = 0; i < barWidth; ++i) {
        const auto [colour, visible] =
            heat(std::max(0.05F, static_cast<float>(i) / static_cast<float>(barWidth - 1)));
        img.draw_rectangle(barLeft + i, barTop, barLeft + i, barTop + px(10), colour, 1.0f);
    }
    const std::string high = std::format("{:.1f} (99th pct)", map.p99);
    img.draw_text(barLeft + barWidth + px(8), legendY, "%s", ink(textSecondary), 0, 1.0f, &font,
                  high.c_str());
    return img;
}

}  // namespace

/// Read one header integer, skipping whitespace and `#` comments.
int getPpmNextInt(std::istream &in, const std::string &path) {
    for (;;) {
        const int c = in.peek();
        if (c == '#') {
            std::string comment;
            std::getline(in, comment);
            continue;
        }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            in.get();
            continue;
        }
        break;
    }
    int v = 0;
    if (!(in >> v)) {
        throw std::runtime_error(path + ": truncated PPM header");
    }
    return v;
}

/// Read a binary P6 (PPM) still into interleaved RGB.
///
/// CImg's savers emit valid files, but its PNM and BMP *loaders* return garbage
/// for images that CImg itself just wrote (a 3-colour round trip came back as
/// 27 colours), so reading is done here against the file format directly. P6 is
/// a magic number, three integers, one whitespace byte, then raw RGB triples.
void loadPpm6(const std::string &path, int &w, int &h, std::vector<std::uint8_t> &rgb) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    std::string magic;
    if (!(in >> magic) || magic != "P6") {
        throw std::runtime_error(path + ": not a binary PPM");
    }
    w = getPpmNextInt(in, path);
    h = getPpmNextInt(in, path);
    const int maxVal = getPpmNextInt(in, path);
    if (maxVal != 255) {
        throw std::runtime_error(path + ": only 8-bit PPM is supported");
    }
    in.get();  // the single whitespace byte separating header and payload
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3;
    rgb.resize(n);
    in.read(reinterpret_cast<char *>(rgb.data()), static_cast<std::streamsize>(n));
    if (static_cast<std::size_t>(in.gcount()) != n) {
        throw std::runtime_error(path + ": truncated PPM payload");
    }
}

/// Big-endian 32-bit, the byte order every PNG field is stored in.
void putBe32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(v & 0xff));
}

/// One PNG chunk: length, type, payload, CRC over type and payload.
void putChunk(std::vector<std::uint8_t> &out, const char *type, const std::uint8_t *data,
              std::size_t n) {
    putBe32(out, static_cast<std::uint32_t>(n));
    const std::size_t crcStart = out.size();
    out.insert(out.end(), type, type + 4);
    if (n > 0) {
        out.insert(out.end(), data, data + n);
    }
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), out.data() + crcStart,
                            static_cast<uInt>(out.size() - crcStart));
    putBe32(out, static_cast<std::uint32_t>(crc));
}

}  // namespace

// Write interleaved 8-bit RGB @p rgb as a truecolour PNG.// Self-contained, like the GIF writer: the container, the CRC and the deflate are// all produced in-process, and the only dependency is zlib, which the build// already links. CImg cannot do this -- it writes PNG through libpng, whose// headers are not installed here -- and a PPM, the one format the raster path can// always write, is not something a browser or an image viewer will open. So a// final still that is meant to be looked at is written as a PNG instead.// The scanlines are stored with filter type 0 (None). PNG's predictors are a// size optimisation and a placement frame is mostly flat colour, which deflate// already handles; picking a real filter per scanline would buy a few percent for// a pass over every pixel that this image is written exactly once.

bool writePng(const std::string &path, int w, int h, const std::vector<std::uint8_t> &rgb) {
    if (w <= 0 || h <= 0 || rgb.size() < static_cast<std::size_t>(w) * h * 3) {
        return false;
    }
    const std::size_t stride = static_cast<std::size_t>(w) * 3;
    const std::size_t line = stride + 1;
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(h) * line);
    for (int y = 0; y < h; ++y) {
        std::uint8_t *dst = raw.data() + static_cast<std::size_t>(y) * line;
        *dst++ = 0;
        std::memcpy(dst, rgb.data() + static_cast<std::size_t>(y) * stride, stride);
    }

    const std::size_t bands = std::min<std::size_t>(static_cast<std::size_t>(h), pngBands);
    std::vector<std::vector<std::uint8_t>> parts(bands);
    std::vector<uLong> checksums(bands);
    std::vector<std::size_t> lengths(bands);
    std::atomic<bool> failed{false};
    tbb::parallel_for(std::size_t{0}, bands, [&](std::size_t band) {
        const std::size_t first = static_cast<std::size_t>(h) * band / bands * line;
        const std::size_t last = static_cast<std::size_t>(h) * (band + 1) / bands * line;
        const std::size_t length = last - first;
        lengths[band] = length;
        checksums[band] =
            adler32(adler32(0L, Z_NULL, 0), raw.data() + first, static_cast<uInt>(length));
        z_stream stream{};
        if (deflateInit2(&stream, pngLevel, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            failed = true;
            return;
        }
        std::vector<std::uint8_t> &out = parts[band];
        out.resize(deflateBound(&stream, static_cast<uLong>(length)) + 64);
        stream.next_in = raw.data() + first;
        stream.avail_in = static_cast<uInt>(length);
        stream.next_out = out.data();
        stream.avail_out = static_cast<uInt>(out.size());
        const int flush = band + 1 == bands ? Z_FINISH : Z_FULL_FLUSH;
        const int status = deflate(&stream, flush);
        if (status != (flush == Z_FINISH ? Z_STREAM_END : Z_OK) || stream.avail_in != 0) {
            failed = true;
        }
        out.resize(stream.total_out);
        deflateEnd(&stream);
    });
    if (failed) {
        return false;
    }

    std::vector<std::uint8_t> idat{0x78, 0x9c};
    uLong checksum = checksums[0];
    for (std::size_t band = 0; band < bands; ++band) {
        idat.insert(idat.end(), parts[band].begin(), parts[band].end());
        if (band > 0) {
            checksum =
                adler32_combine(checksum, checksums[band], static_cast<z_off_t>(lengths[band]));
        }
    }
    putBe32(idat, static_cast<std::uint32_t>(checksum));

    std::vector<std::uint8_t> ihdr;
    putBe32(ihdr, static_cast<std::uint32_t>(w));
    putBe32(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    putChunk(png, "IHDR", ihdr.data(), ihdr.size());
    putChunk(png, "IDAT", idat.data(), idat.size());
    putChunk(png, "IEND", nullptr, 0);

    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        return false;
    }
    out.write(reinterpret_cast<const char *>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

bool writeImagePng(const std::string &path, const CImg<unsigned char> &img) {
    const auto w = static_cast<std::size_t>(img.width());
    std::vector<std::uint8_t> rgb(w * static_cast<std::size_t>(img.height()) * 3);
    tbb::parallel_for(0, img.height(), [&](int py) {
        std::uint8_t *out = rgb.data() + static_cast<std::size_t>(py) * w * 3;
        for (int ch = 0; ch < 3; ++ch) {
            const unsigned char *in = img.data(0, py, 0, ch);
            for (std::size_t px = 0; px < w; ++px) {
                out[px * 3 + ch] = in[px];
            }
        }
    });
    return writePng(path, img.width(), img.height(), rgb);
}

bool writeBoundsPng(const std::string &path, std::string_view title,
                    std::span<const BoundsSample> curve) {
    constexpr int chartW = 900;
    constexpr int chartH = 420;
    constexpr int left = 110;
    constexpr int right = 30;
    constexpr int top = 50;
    constexpr int bottom = 50;
    const unsigned char ink[] = {40, 40, 40};
    const unsigned char grid[] = {225, 225, 225};
    const unsigned char lowerInk[] = {36, 113, 163};
    const unsigned char upperInk[] = {192, 57, 43};

    double highest = 1e-12;
    for (const BoundsSample &sample : curve) {
        highest = std::max({highest, sample.upper, sample.lower});
    }
    const auto px = [&](std::size_t i) {
        const double t =
            curve.size() < 2 ? 0.0 : static_cast<double>(i) / static_cast<double>(curve.size() - 1);
        return static_cast<int>(std::lround(left + t * (chartW - left - right)));
    };
    const auto py = [&](double v) {
        const double t = std::clamp(v / highest, 0.0, 1.0);
        return static_cast<int>(std::lround(chartH - bottom - t * (chartH - top - bottom)));
    };

    CImg<unsigned char> img(chartW, chartH, 1, 3, 255);
    for (int k = 0; k <= 4; ++k) {
        const double v = highest * k / 4.0;
        img.draw_line(left, py(v), chartW - right, py(v), grid);
        const std::string label = std::format("{:.3e}", v);
        img.draw_text(8, py(v) - 7, "%s", ink, 0, 1.0f, 14, label.c_str());
    }
    img.draw_line(left, chartH - bottom, chartW - right, chartH - bottom, ink);
    img.draw_line(left, top, left, chartH - bottom, ink);
    for (std::size_t i = 1; i < curve.size(); ++i) {
        for (int thick = 0; thick < 2; ++thick) {
            img.draw_line(px(i - 1), py(curve[i - 1].lower) + thick, px(i),
                          py(curve[i].lower) + thick, lowerInk);
            img.draw_line(px(i - 1), py(curve[i - 1].upper) + thick, px(i),
                          py(curve[i].upper) + thick, upperInk);
        }
    }
    const std::string heading(title);
    img.draw_text(left, 14, "%s", ink, 0, 1.0f, 20, heading.c_str());
    const std::string axis =
        std::format("iteration (0 .. {})", curve.empty() ? 0 : curve.size() - 1);
    img.draw_text(left, chartH - bottom + 14, "%s", ink, 0, 1.0f, 14, axis.c_str());
    img.draw_text(chartW - right - 150, top + 6, "%s", lowerInk, 0, 1.0f, 16, "lower bound");
    img.draw_text(chartW - right - 150, top + 26, "%s", upperInk, 0, 1.0f, 16, "upper bound");
    return writeImagePng(path, img);
}

bool ensureDir(const std::string &dir) {
    if (dir.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

BBox getFixedCellBBox(const Graph &g) {
    // The "die" is taken as the bounding box of all fixed/terminal cells.
    // Vertices store their lower-left corner, so the box must also include
    // corner + (width, height): pads anchored at the right/top rim would
    // otherwise overhang the drawn die rectangle.
    const std::size_t nv = g.getNumCells();
    BBox fixed = BBox::getEmpty();
    BBox all = BBox::getEmpty();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        const BBox cell = BBox::of(vert.position.x, vert.position.y, std::max(vert.width, 1.0),
                                   std::max(vert.height, 1.0));
        all.extend(cell);
        if (vert.isFixed || vert.isTerminal) {
            fixed.extend(cell);
        }
    }
    if (fixed.isEmpty()) {
        // No fixed cells at all. Returning a 1x1 box here is a trap: the
        // viewport is built from this box, so every cell of a design whose real
        // extent is 1e5 units falls outside it and the frame comes out empty --
        // a blank picture that looks like a renderer failure rather than a
        // missing fallback. Several public Bookshelf designs have no pads, and
        // the legalized frame of such a run drew nothing at all.
        //
        // The sensible reading of "the die" for a design with no fixed geometry is
        // the extent of the design itself, so fall back to every cell. Callers
        // that know the row structure pass a real core box anyway; this only has
        // to be good enough to frame the drawing.
        if (all.isEmpty()) {
            return BBox(Point(0.0, 0.0), Point(1.0, 1.0));  // genuinely empty graph
        }
        return all;
    }
    return fixed;
}

void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note, const constraintMgr *constraints, bool fixedView,
                   bool worldUnits) {
    const std::size_t nv = g.getNumCells();
    // By default the view auto-fits the data, which is right for a single frame but
    // makes a sequence impossible to read: a collapsed iteration 0 and a spread
    // iteration 9 are drawn at different scales, so the eye compares zoom levels
    // rather than placements. fixedView pins the viewport to the die so every
    // frame in a sequence is directly comparable.
    // SVG is resolution-independent, so it always draws at zoom 1; zoom applies to
    // the raster path, where the pixel size is set at render time.
    //
    // worldUnits writes the final still in world units with the die as the viewBox,
    // rather than pre-scaling it to 768. SVG is resolution independent, so baking a
    // frame-size scale into the coordinates buys nothing and costs the precision:
    // at 768 over a 10692-unit die one site is 0.07 of a unit, which one decimal
    // place cannot represent, so cells a site apart round onto each other and the
    // "exact one rect per cell" artefact stops being exact. In world units it is
    // exact at any size, and still opens at a sensible size because width/height
    // carry the on-screen size separately from the viewBox.
    ViewPort vp = fixedView ? getDieViewPort(dieBox, /*zoom=*/1.0)
                            : makeViewPort(g, x, y, dieBox, /*zoom=*/1.0);
    double vbX = 0.0, vbY = 0.0, vbW = imageW, vbH = imageH;
    int prec = 1;
    if (worldUnits) {
        const double spanX = dieBox.getWidth();
        const double spanY = dieBox.getHeight();
        vp.lowerLeft.x = dieBox.lowerLeft.x;
        vp.lowerLeft.y = dieBox.lowerLeft.y;
        vp.sx = 1.0;
        vp.sy = 1.0;
        vp.margin = 0.0;
        vp.height = spanY;
        // toPxX and toPxY already subtract the die's lower-left corner, so the
        // emitted coordinates already run 0..span. The viewBox has to start at the
        // origin to match, or the whole drawing sits one die offset off-canvas.
        vbX = 0.0;
        vbY = 0.0;
        vbW = spanX;
        vbH = spanY;
        prec = 3;  // well under a site at the finest pitch these designs use
    }

    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << R"(<svg xmlns="http://www.w3.org/2000/svg" width=")" << imageW << "\" height=\""
        << imageH << "\" viewBox=\"" << getFmt(vbX, prec) << " " << getFmt(vbY, prec) << " "
        << getFmt(vbW, prec) << " " << getFmt(vbH, prec) << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<title>step " << step << ": " << note << "</title>\n";

    const double pct = numSteps > 1
                           ? 100.0 * static_cast<double>(step) / static_cast<double>(numSteps - 1)
                           : 100.0;

    // Progress bar.
    // No progress bar, for the same reason as the raster frames: it sits along the
    // bottom of the frame, which is where the placement is, and on a design whose
    // cells reach the bottom rows it covers them. The iteration number is in the
    // caption and the per-iteration CSVs carry the series in numbers.
    out << "<rect x=\"" << margin / 3 << "\" y=\"" << imageH - 14 << "\" width=\""
        << (imageW - 2 * margin / 3) * pct / 100.0 << "\" height=\"6\" fill=\"#4fc3f7\"/>\n";

    // Die (fixed-pad) frame.
    out << "<rect x=\"" << getFmt(toPxX(vp, dieBox.lowerLeft.x), prec) << "\" y=\""
        << getFmt(toPxY(vp, dieBox.upperRight.y), prec) << "\" width=\""
        << getFmt((dieBox.getWidth()) * vp.sx) << "\" height=\""
        << getFmt((dieBox.getHeight()) * vp.sy)
        << "\" fill=\"none\" stroke=\"#bdbdbd\" stroke-width=\"1\"/>\n";

    // The rows, under the cells, as in the raster frame. Recovered from the cells
    // rather than passed in, so a frame of a design with no row structure -- an
    // unlegalized placement -- simply has none.
    const std::vector<std::array<double, 2>> rows = getRowBands(g, y);
    if (!rows.empty()) {
        // Every eighth row, for the same reason as in the raster renderer: a line
        // at every pitch hatches the whole placement.
        constexpr std::size_t every = 8;
        out << "<g stroke=\"#8fa3b8\" stroke-opacity=\"0.34\" stroke-width=\"1\">\n";
        for (std::size_t i = 0; i < rows.size(); i += every) {
            out << "<line x1=\"" << getFmt(toPxX(vp, dieBox.lowerLeft.x), prec) << "\" y1=\""
                << getFmt(toPxY(vp, rows[i][1]), prec) << "\" x2=\""
                << getFmt(toPxX(vp, dieBox.upperRight.x), prec) << "\" y2=\""
                << getFmt(toPxY(vp, rows[i][1]), prec) << "\"/>\n";
        }
        out << "</g>\n";
    }

    // Fence regions, drawn under the cells so the placement stays readable.
    // Each region is a union of rectangles, so every piece is outlined and
    // filled; the name is labelled at the region's lower-left corner.
    if (constraints != nullptr) {
        static const char *fenceColors[] = {"#ffb74d", "#ba68c8", "#4db6ac", "#f06292",
                                            "#9575cd", "#ffd54f", "#4fc3f7", "#a1887f"};
        for (std::size_t ri = 0; ri < constraints->getNumRegions(); ++ri) {
            const Region &reg = *constraints->getRegion(static_cast<int>(ri));
            const char *color = fenceColors[ri % (sizeof(fenceColors) / sizeof(char *))];
            for (const BBox &r : reg.rects) {
                out << "<rect x=\"" << getFmt(toPxX(vp, r.lowerLeft.x), prec) << "\" y=\""
                    << getFmt(toPxY(vp, r.upperRight.y), prec) << "\" width=\""
                    << getFmt((r.getWidth()) * vp.sx) << "\" height=\""
                    << getFmt((r.getHeight()) * vp.sy) << "\" fill=\"" << color
                    << R"(" fill-opacity="0.13" stroke=")" << color
                    << "\" stroke-width=\"1.5\" stroke-opacity=\"0.9\"/>\n";
            }
            out << "<text x=\"" << getFmt(toPxX(vp, reg.bounds.lowerLeft.x) + 3) << "\" y=\""
                << getFmt(toPxY(vp, reg.bounds.lowerLeft.y) - 3) << "\" fill=\"" << color
                << R"(" font-family="monospace" font-size="11">)" << reg.name << "</text>\n";
        }
    }

    // Fixed macros: never decimate, so hard cells match the DEF floorplan.
    // Fixed macros: near-white, matching the raster renderer, so a macro is
    // outside the density ramp in both representations of a frame.
    out << R"(<g fill="#ef5350" stroke="#5c1a1a" stroke-opacity="0.75")"
        << " stroke-width=\"0.7\">\n";
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        if (!vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << getFmt(toPxX(vp, x[v]), prec) << "\" y=\""
            << getFmt(toPxY(vp, y[v] + vert.height), prec) << "\" width=\"" << getFmt(w, prec)
            << "\" height=\"" << getFmt(h, prec) << "\"/>\n";
    }
    // I/O pads / terminals: never decimated; every pad is drawn.
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        if (!vert.isTerminal || vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << getFmt(toPxX(vp, x[v]), prec) << "\" y=\""
            << getFmt(toPxY(vp, y[v] + vert.height), prec) << "\" width=\"" << getFmt(w, prec)
            << "\" height=\"" << getFmt(h, prec) << "\"/>\n";
    }
    // Movable cells, one flat blue, matching the raster renderer so the two
    // representations of a frame are the same picture. One <g> for the whole set
    // rather than one per cell: a 200k-cell frame with per-cell grouping carries
    // 200k elements of markup for nothing.
    //
    // The rim is what keeps abutting cells apart. A legal placement is cells
    // touching exactly, so without it a whole region reads as one blue shape; the
    // opacity is kept low and the width small, because a heavy outline turns the
    // placement into a diagram of boxes rather than a picture of cells.
    //
    // This is the bulk of a frame -- one line per cell, and every conjugate-
    // gradient iterate writes a frame -- so the lines are formatted in parallel,
    // one string per fixed-size chunk of cells, and the chunks are written in
    // cell order. The file is byte-for-byte what a serial loop writes.
    {
        out << R"(<g fill="#4fc3f7" stroke="#0b3d54" stroke-opacity="0.75")"
            << " stroke-width=\"0.6\">\n";
        constexpr std::size_t chunkSize = 4096;
        std::vector<std::string> chunks((nv + chunkSize - 1) / chunkSize);
        tbb::parallel_for(std::size_t{0}, chunks.size(), [&](std::size_t c) {
            std::string &text = chunks[c];
            const std::size_t end = std::min(nv, (c + 1) * chunkSize);
            for (std::size_t v = c * chunkSize; v < end; ++v) {
                const Vertex &vert = g.getCell(v);
                if (vert.isFixed || vert.isTerminal) {
                    continue;
                }
                // Exact size, as in the raster path, so the two agree and so a cell
                // is never drawn wider than it is.
                const double w = std::max(0.75, vert.width * vp.sx);
                const double h = std::max(0.75, vert.height * vp.sy);
                std::format_to(std::back_inserter(text),
                               "<rect x=\"{}\" y=\"{}\" width=\"{}\" height=\"{}\"/>\n",
                               getFmt(toPxX(vp, x[v]), prec),
                               getFmt(toPxY(vp, y[v] + vert.height), prec), getFmt(w, prec),
                               getFmt(h, prec));
            }
        });
        for (const std::string &text : chunks) {
            out << text;
        }
        out << "</g>\n";
    }

    out << "</g>\n";

    out << "<text x=\"" << margin / 3
        << R"(" y="24" fill="#ffffff" font-family="monospace" font-size="14">)" << "step " << step
        << (numSteps > 0 ? " / " + std::to_string(numSteps - 1) : std::string()) << " — " << note
        << "</text>\n";
    // Omitted when unmeasured, for the same reason as the raster caption.
    if (hpwl > 0.0) {
        out << "<text x=\"" << margin / 3
            << R"(" y="44" fill="#90caf9" font-family="monospace" font-size="13">)"
            << "HPWL = " << getFmt(hpwl, prec)
            << (hpwlInitial > 0.0 ? "  (initial " + getFmt(hpwlInitial, prec) + ")" : "")
            << "</text>\n";
    }
    if (resid > 0.0) {
        out << "<text x=\"" << margin / 3
            << R"(" y="62" fill="#ffeb3b" font-family="monospace" font-size="12">)"
            << "density overflow = " << getSci(resid) << "</text>\n";
    }
    // Legend: a swatch and a count per category, matching the raster frame.
    {
        const int legendY = resid > 0.0 ? 80 : 62;
        int cx = static_cast<int>(margin / 3);
        std::size_t movable = 0, fixed = 0, pads = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (vert.isFixed) {
                ++fixed;
            } else if (vert.isTerminal) {
                ++pads;
            } else {
                ++movable;
            }
        }
        const auto swatch = [&](const char *fill, const std::string &label, std::size_t count) {
            out << "<rect x=\"" << cx << "\" y=\"" << legendY - 10 << R"(" width="10")"
                << R"( height="12" fill=")" << fill << "\"/>\n";
            cx += 16;
            const std::string text = label + " " + std::to_string(count);
            out << "<text x=\"" << cx << "\" y=\"" << legendY
                << R"(" fill="#90caf9" font-family="monospace" font-size="12">)" << text
                << "</text>\n";
            cx += static_cast<int>(text.size()) * 7 + 14;
        };
        swatch("#4fc3f7", "movable", movable);
        // Only categories that exist; see the note in the raster renderer.
        if (fixed > 0) {
            swatch("#ef5350", "fixed macro", fixed);
        }
        if (pads > 0) {
            swatch("#ef5350", "I/O pad", pads);
        }
        if (!rows.empty()) {
            out << "<rect x=\"" << cx << "\" y=\"" << legendY - 10
                << R"(" width="10" height="12" fill="none" stroke="#8fa3b8")"
                << " stroke-opacity=\"0.5\"/>\n";
            cx += 16;
            const std::string text = "row (" + std::to_string(rows.size()) + ")";
            out << "<text x=\"" << cx << "\" y=\"" << legendY
                << R"(" fill="#90caf9" font-family="monospace" font-size="12">)" << text
                << "</text>\n";
        }
    }
    out << "</svg>\n";
    out.close();
}

void writeFrameRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                      const std::vector<float> &y, const BBox &dieBox, double hpwl,
                      double hpwlInitial, double resid, const std::string &note,
                      const constraintMgr *constraints, bool fixedView, double zoom, bool drawRows,
                      const std::string &heading) {
    // A throwaway palette is fine for a single still; writeAnimatedGif() is the
    // path that needs one palette shared by every frame.
    GifPalette pal;
    const CImg<unsigned char> img =
        renderFrameCImg(g, x, y, dieBox, hpwl, hpwlInitial, resid, note, constraints, fixedView,
                        pal, zoom, drawRows, heading);
    // Dispatch on the extension. ".ppm" and ".bmp" are handled natively by CImg
    // and need no external library. ".png" is written here rather than handed to
    // CImg, whose PNG support needs libpng headers this build does not have.
    if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".png") == 0) {
        if (!writeImagePng(path, img)) {
            ktlog.warning("cannot write '{}'", path);
        }
        return;
    }
    img.save(path.c_str());
}

void writeDemandRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                       const std::vector<float> &y, const BBox &dieBox, const std::string &note,
                       const std::string &heading, double zoom, std::size_t bins) {
    GifPalette pal;
    renderDemandCImg(g, x, y, dieBox, note, heading, pal, zoom, bins).save(path.c_str());
}

void writeFinalFrameRaster(const std::string &path, const Graph &g,
                           const constraintMgr *constraints, double zoom, double hpwl) {
    // The finished placement lives in the vertices' stored coordinates, which is
    // what the frame writers get passed as x/y; here the graph is the payload.
    const std::size_t nv = g.getNumCells();
    std::vector<float> x(nv), y(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        x[v] = static_cast<float>(g.getCell(v).position.x);
        y[v] = static_cast<float>(g.getCell(v).position.y);
    }
    const BBox die = getFixedCellBBox(g);
    // fixedView pins the viewport to the die, which for one final still is the
    // right frame: the anchor scene the whole run has been using.
    // The finished placement has no "initial" to compare against, so none is
    // passed: the caption then shows one number instead of repeating one.
    writeFrameRaster(path, g, x, y, die, hpwl, /*hpwlInitial=*/0.0, 0.0,
                     "Legalized and detail-placed", constraints, /*fixedView=*/true, zoom,
                     /*drawRows=*/true, "Final placement");
}

namespace {
// The stills one stage contributed, in frame order. Names are zero-padded, so
// lexical order is frame order.
std::vector<std::string> collectFrames(const std::string &dir) {
    // Reading the frames back rather than buffering them keeps memory flat over
    // a long run, and the stills stay on disk as a browsable fallback. Only PPM
    // is accepted: it is the one format here that can be written and read back
    // correctly.
    static const char *exts[] = {".ppm"};
    std::vector<std::string> frames;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(framePrefix)) {
            continue;
        }
        for (const char *ext : exts) {
            const std::size_t n = std::strlen(ext);
            if (name.size() > n && name.compare(name.size() - n, n, ext) == 0) {
                frames.push_back(entry.path().string());
                break;
            }
        }
    }
    std::sort(frames.begin(), frames.end());
    return frames;
}
}  // namespace

bool writeAnimatedGif(const std::string &dir, const std::string &gifPath, int delayCs) {
    const std::vector<std::string> frames = collectFrames(dir);
    if (frames.empty()) {
        return false;
    }

    // GIF stores palette indices, so every frame indexes one shared 256-entry
    // table. A placement frame does not fit in 256 colours on its own -- the
    // antialiased edges of a dense placement, the rim that separates abutting
    // cells, and the antialiased legend text together run to several hundred
    // distinct shades -- so the table is built first, by median cut over a sample
    // of every frame, and only then is each pixel mapped to its nearest entry.
    //
    // The alternative, assigning an index to each colour as it is first seen and
    // falling back to "closest so far" once the table fills, is what made an
    // earlier build's animation shimmer: the table filled up partway through, so
    // early frames got exact entries and later ones got approximations of the
    // same colours, and a cell that had not moved changed shade between frames.
    // Choosing the palette up front makes the mapping a pure function of the
    // pixel, so identical pixels are identical in every frame by construction.
    constexpr std::size_t maxPalette = 256;
    // Sample every Nth pixel of every Nth frame. The palette only needs to
    // represent the distribution, not every pixel, and a full pass over 300 frames
    // of 768x768 is 176M pixels to look at.
    constexpr std::size_t sampleStride = 5;
    constexpr std::size_t frameStride = 3;

    struct Frame {
        int w = 0, h = 0;
        bool ok = false;
        std::vector<std::uint8_t> idx;
        std::vector<std::uint8_t> rgb;
        std::map<std::uint32_t, std::uint32_t> histogram;
        std::vector<std::uint32_t> firstSeen;
    };
    std::vector<Frame> loaded(frames.size());
    tbb::enumerable_thread_specific<std::vector<std::uint64_t>> seenBits([] {
        return std::vector<std::uint64_t>((1u << 24) / 64, 0);
    });
    tbb::parallel_for(std::size_t{0}, frames.size(), [&](std::size_t fi) {
        Frame &f = loaded[fi];
        try {
            loadPpm6(frames[fi], f.w, f.h, f.rgb);
        } catch (const std::exception &) {
            return;
        }
        f.ok = true;
        const std::size_t np = f.rgb.size() / 3;
        if (fi % frameStride == 0) {
            for (std::size_t i = 0; i < np; i += sampleStride) {
                ++f.histogram[packRgb(f.rgb[3 * i], f.rgb[3 * i + 1], f.rgb[3 * i + 2])];
            }
        }
        std::vector<std::uint64_t> &seen = seenBits.local();
        for (std::size_t i = 0; i < np; ++i) {
            const std::uint32_t key = packRgb(f.rgb[3 * i], f.rgb[3 * i + 1], f.rgb[3 * i + 2]);
            const std::uint64_t bit = std::uint64_t{1} << (key & 63u);
            if ((seen[key >> 6] & bit) == 0) {
                seen[key >> 6] |= bit;
                f.firstSeen.push_back(key);
            }
        }
        for (const std::uint32_t key : f.firstSeen) {
            seen[key >> 6] = 0;
        }
    });
    std::erase_if(loaded, [](const Frame &f) {
        return !f.ok;
    });
    if (loaded.empty()) {
        return false;
    }

    std::map<std::uint32_t, std::uint32_t> histogram;
    for (const Frame &f : loaded) {
        for (const auto &[key, count] : f.histogram) {
            histogram[key] += count;
        }
    }

    const auto unpack = [](std::uint32_t key) {
        return Rgb24{.r = static_cast<std::uint8_t>((key >> 16) & 0xff),
                     .g = static_cast<std::uint8_t>((key >> 8) & 0xff),
                     .b = static_cast<std::uint8_t>(key & 0xff)};
    };
    GifPalette pal;
    for (const Rgb24 &c : getMedianCutPalette(histogram, maxPalette)) {
        pal.getIndex(c);
    }
    // Any colour the sample missed still needs an entry, or it would be mapped to
    // whatever happened to be nearest. Appending the remainder keeps the mapping
    // exact where it can be and only approximate at the long tail.
    if (pal.getColors().size() < maxPalette) {
        for (const auto &[key, count] : histogram) {
            (void)count;
            pal.getIndex(unpack(key));
            if (pal.getColors().size() >= maxPalette) {
                break;
            }
        }
    }

    std::vector<std::uint8_t> indexOf(1u << 24, 0);
    std::vector<std::uint64_t> assigned((1u << 24) / 64, 0);
    for (const Frame &f : loaded) {
        for (const std::uint32_t key : f.firstSeen) {
            const std::uint64_t bit = std::uint64_t{1} << (key & 63u);
            if ((assigned[key >> 6] & bit) == 0) {
                assigned[key >> 6] |= bit;
                indexOf[key] = pal.getIndex(unpack(key));
            }
        }
    }

    tbb::parallel_for(std::size_t{0}, loaded.size(), [&](std::size_t fi) {
        Frame &f = loaded[fi];
        const std::size_t np = f.rgb.size() / 3;
        f.idx.resize(np);
        for (std::size_t i = 0; i < np; ++i) {
            f.idx[i] = indexOf[packRgb(f.rgb[3 * i], f.rgb[3 * i + 1], f.rgb[3 * i + 2])];
        }
        f.rgb.clear();
        f.rgb.shrink_to_fit();
    });
    std::vector<Frame> &quantised = loaded;

    std::vector<Rgb> palRgb;
    palRgb.reserve(pal.getColors().size());
    for (const Rgb24 &c : pal.getColors()) {
        palRgb.push_back(Rgb{.r = c.r, .g = c.g, .b = c.b});
    }

    std::vector<Canvas> canvases;
    canvases.reserve(quantised.size());
    for (const Frame &f : quantised) {
        canvases.emplace_back(f.w, f.h, f.idx, palRgb);
    }

    if (!writeGif(gifPath, canvases, delayCs)) {
        return false;
    }
    // The stills were scratch for the encoder, not a deliverable: a 300-frame
    // animation leaves 300 P6 files that are several hundred megabytes and that
    // nothing can open anyway. They are removed once the GIF they were built into
    // is safely on disk, and only then -- if the encode failed the frames are all
    // that is left of the run's per-frame record, so they stay. Removal is per
    // file and failures are ignored: a locked file must not turn a written GIF
    // into a reported failure.
    for (const std::string &f : frames) {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    return true;
}

}  // namespace ktplace
