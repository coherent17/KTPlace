/**
 * @file kt_plotter.cc
 * @brief Implementation of the SVG/HTML/CSV/PNG/GIF placement visualization helpers
 */

#include "visualization/kt_plotter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// CImg is a header-only library, so it is confined to this translation unit.
// Drawing is done in RGB and then mapped onto a small fixed palette, which is
// what GIF requires and what keeps every frame of an animation colour-identical.
#define cimg_display 0
#define cimg_verbosity 0
#include "visualization/CImg.h"

#include "visualization/kt_gif.h"

using cimg_library::CImg;
using cimg_library::CImgList;

namespace ktplace {

namespace {

constexpr std::size_t kMaxPoints = 150000;  // max dots per frame
constexpr double kMargin = 36.0;            // image margin in pixels
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
constexpr double kHeaderH = 116.0;
constexpr double kImageW = 768.0;           // frame image size
constexpr double kImageH = 768.0;

/// Filename stem of the per-iteration stills that writeAnimatedGif() collects.
constexpr const char *kFramePrefix = "frame_";

std::string fmt(double v, int prec = 1) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

std::string sci(double v) {
    std::ostringstream os;
    os << std::scientific << std::setprecision(1) << v;
    return os.str();
}

struct ViewPort {
    double minX = 0.0, minY = 0.0, maxX = 1.0, maxY = 1.0;
    double sx = 1.0, sy = 1.0;  // units -> px
};

/// Scale that fits a spanX-by-spanY die into the drawing area: the full width less
/// the side margins, and the height less the header band and the bottom margin.
double fitScale(double spanX, double spanY) {
    const double availW = kImageW - 2.0 * kMargin - 2.0;
    const double availH = kImageH - kHeaderH - kMargin - 2.0;
    // The viewports pad minX/minY by 1% of the span, so the extent actually drawn
    // is 1.02 spans wide. Dividing by the padded span is what makes the die land
    // inside the area rather than 2% proud of it -- and since the die is anchored
    // to the bottom, "2% proud" is 2% *upward*, straight back under the caption.
    constexpr double kPad = 1.02;
    return std::min(availW, availH) / (kPad * std::max(std::max(spanX, spanY), 1.0));
}

ViewPort makeViewPort(const Graph &g, const std::vector<float> &x, const std::vector<float> &y,
                      const BBox &dieBox) {
    const std::size_t nv = g.getNumVertices();
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();
    for (std::size_t v = 0; v < nv; ++v) {
        minX = std::min(minX, static_cast<double>(x[v]));
        minY = std::min(minY, static_cast<double>(y[v]));
        maxX = std::max(maxX, static_cast<double>(x[v]));
        maxY = std::max(maxY, static_cast<double>(y[v]));
    }
    minX = std::min(minX, dieBox[0]);
    minY = std::min(minY, dieBox[1]);
    maxX = std::max(maxX, dieBox[2]);
    maxY = std::max(maxY, dieBox[3]);
    const double spanX = std::max(maxX - minX, 1.0);
    const double spanY = std::max(maxY - minY, 1.0);
    const double sc = fitScale(spanX, spanY);
    ViewPort vp;
    vp.minX = minX - 0.01 * spanX;
    vp.minY = minY - 0.01 * spanY;
    vp.sx = sc;
    vp.sy = sc;
    return vp;
}

/// Viewport spanning exactly the die, so a series of frames shares one scale.
ViewPort dieViewPort(const BBox &dieBox) {
    ViewPort vp;
    const double spanX = std::max(dieBox[2] - dieBox[0], 1.0);
    const double spanY = std::max(dieBox[3] - dieBox[1], 1.0);
    const double sc = fitScale(spanX, spanY);
    vp.minX = dieBox[0] - 0.01 * spanX;
    vp.minY = dieBox[1] - 0.01 * spanY;
    vp.sx = sc;
    vp.sy = sc;
    return vp;
}

double toPxX(const ViewPort &vp, double v) {
    return kMargin + (v - vp.minX) * vp.sx;
}
double toPxY(const ViewPort &vp, double v) {
    // Offset by the header so the die starts below the caption instead of under it.
    return kImageH - kMargin - (v - vp.minY) * vp.sy;
}

// ---------------------------------------------------------------------------
// Raster (CImg) rendering
// ---------------------------------------------------------------------------

/// 24-bit colour, matching the format GIF's palette stores.
struct Rgb24 {
    std::uint8_t r = 0, g = 0, b = 0;
    /// The three channels as an array, for filling an image a channel at a time.
    std::array<std::uint8_t, 3> rgb() const {
        return {r, g, b};
    }
};

constexpr Rgb24 kBgColor{0x10, 0x14, 0x18};

/// Parse "#rrggbb" as used throughout the SVG renderer.
Rgb24 hexColor(const char *s) {
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
    return Rgb24{static_cast<std::uint8_t>(std::lround(c.r * f)),
                 static_cast<std::uint8_t>(std::lround(c.g * f)),
                 static_cast<std::uint8_t>(std::lround(c.b * f))};
}

Rgb24 blendOnBg(const Rgb24 &c, double alpha) {
    // The three background channels differ, so blend each against its own value.
    auto chan = [alpha](std::uint8_t s, std::uint8_t bg) {
        return static_cast<std::uint8_t>(std::lround(alpha * s + (1.0 - alpha) * bg));
    };
    Rgb24 o;
    o.r = chan(c.r, kBgColor.r);
    o.g = chan(c.g, kBgColor.g);
    o.b = chan(c.b, kBgColor.b);
    return o;
}

/**
 * @brief Shared colour table for a whole animation.
 *
 * GIF stores palette indices, so an animation is only correct if every frame
 * indexes the same table. Indices are therefore handed out on first use and the
 * table is kept alive across all frames; the renderer asks for a colour by value
 * and always gets the same index back.
 */
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
std::vector<Rgb24> medianCutPalette(const std::map<std::uint32_t, std::uint32_t> &hist,
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
            return Rgb24{0, 0, 0};
        }
        return Rgb24{static_cast<std::uint8_t>(std::lround(r / w)),
                     static_cast<std::uint8_t>(std::lround(g / w)),
                     static_cast<std::uint8_t>(std::lround(b / w))};
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
            const double score = static_cast<double>(span) *
                                 static_cast<double>(boxes[i].entries.size());
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
    std::uint8_t index(const Rgb24 &c) {
        const std::uint32_t key = pack(c);
        auto it = lookup_.find(key);
        if (it != lookup_.end()) {
            return it->second;
        }
        if (colors_.size() < 256) {
            const auto idx = static_cast<std::uint8_t>(colors_.size());
            colors_.push_back(c);
            lookup_.emplace(key, idx);
            return idx;
        }
        return nearest(c);
    }

    const std::vector<Rgb24> &colors() const {
        return colors_;
    }

    /// CImg colour pointer (cimg::spectrum() consecutive values) for drawing.
    /// The slot is (re)written on every call, which is idempotent because
    /// index() is stable for a given colour.
    const std::uint8_t *ptr(const Rgb24 &c) {
        const std::size_t i = index(c);
        lut_[3 * i + 0] = c.r;
        lut_[3 * i + 1] = c.g;
        lut_[3 * i + 2] = c.b;
        return lut_.data() + 3 * i;
    }

    /// The same colour as a 3-value image, for fill(), which takes values
    /// rather than a pointer.
    CImg<unsigned char> value(const Rgb24 &c) {
        return CImg<unsigned char>(ptr(c), 3, 1, 1);
    }

private:
    static std::uint32_t pack(const Rgb24 &c) {
        return (static_cast<std::uint32_t>(c.r) << 16) | (static_cast<std::uint32_t>(c.g) << 8) |
               static_cast<std::uint32_t>(c.b);
    }

    std::uint8_t nearest(const Rgb24 &c) const {
        std::size_t best = 0;
        long bestD = std::numeric_limits<long>::max();
        for (std::size_t i = 0; i < colors_.size(); ++i) {
            const long dr = colors_[i].r - c.r, dg = colors_[i].g - c.g, db = colors_[i].b - c.b;
            const long d = dr * dr + dg * dg + db * db;
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return static_cast<std::uint8_t>(best);
    }

    std::vector<Rgb24> colors_;
    std::vector<std::uint8_t> lut_ = std::vector<std::uint8_t>(256 * 3, 0);
    std::unordered_map<std::uint32_t, std::uint8_t> lookup_;
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
int textWidth(const CImgList<unsigned char> &font, const char *s) {
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

/**
 * @brief Raster counterpart of writeFrameSvg(), drawn with CImg.
 *
 * Mirrors the SVG renderer's layout, colours and draw order so the two
 * representations of a frame agree. Everything is drawn flat (see blendOnBg)
 * so the result maps onto the GIF palette without any colour reduction.
 */
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
            constexpr double kAaLevels = 6.0;
            double a = alpha * cx * cy;
            a = std::round(a * kAaLevels) / kAaLevels;
            if (a <= 0.0) {
                continue;
            }
            a = std::max(a, 1.0 / kAaLevels);
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
    constexpr double kAaMinPixels = 4.0;
    const bool big = (x1 - x0) >= kAaMinPixels && (yBot - yTop) >= kAaMinPixels;
    if (!big) {
        fillRectAA(img, std::round(x0), std::round(yTop), std::round(x1), std::round(yBot), rgb,
                   1.0);
        return;
    }
    fillRectAA(img, x0, yTop, x1, yBot, rgb, 1.0);
    // A darker rim, inset half a pixel so it lies inside the cell rather than
    // eating into its neighbour.
    //
    // Standard cells in a dense placement abut each other exactly -- that is what
    // legal placement means -- so with one flat fill the eye cannot find where one
    // cell stops and the next begins, and a whole region reads as a single blob.
    // The rim is what makes a packed placement legible: it costs one outline per
    // cell and it is the difference between "a picture of a placement" and "a
    // picture of cells".
    strokeRectAA(img, x0 + 0.5, yTop + 0.5, x1 - 0.5, yBot - 0.5, 1.0, rim, 1.0);
}
}  // namespace

CImg<unsigned char> renderFrameCImg(const Graph &g, const std::vector<float> &x,
                                    const std::vector<float> &y, const BBox &dieBox,
                                    std::size_t step, std::size_t numSteps, double hpwl,
                                    double hpwlInitial, double resid, const std::string &note,
                                    const constraintMgr *constraints, bool fixedView,
                                    GifPalette &pal) {
    const std::size_t nv = g.getNumVertices();
    const ViewPort vp = fixedView ? dieViewPort(dieBox) : makeViewPort(g, x, y, dieBox);
    const std::size_t stride = (nv > kMaxPoints) ? ((nv + kMaxPoints - 1) / kMaxPoints) : 1;

    CImg<unsigned char> img(static_cast<int>(kImageW), static_cast<int>(kImageH), 1, 3);
    // Fill the background channel by channel. CImg's fill(values, true) cannot be
    // used here: its repeat loop never advances the source pointer, so everything
    // past the first pixel would repeat the red channel and the image would come
    // out grey (r,r,r) instead of the intended background colour.
    const std::array<std::uint8_t, 3> bgv = kBgColor.rgb();
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
        pixelSpan(toPxX(vp, dieBox[0]), toPxX(vp, dieBox[2]), x0, x1);
        pixelSpan(toPxY(vp, dieBox[3]), toPxY(vp, dieBox[1]), y0, y1);
        // A pad the frame is drawn inside, so the band is a band rather than a
        // stroke over the outermost row of cells.
        const int inset = 2;
        const int gx0 = x0 + inset, gy0 = y0 + inset;
        const int gx1 = x1 - inset, gy1 = y1 - inset;
        // Outside: a black keyline, so the boundary separates from anything
        // behind it. Inside: near-white, so it reads as a wall. Both antialiased,
        // because a border is exactly the thing the eye uses to judge whether the
        // placement inside it is aligned, and a staircase there is very visible.
        strokeRectAA(img, x0 - 1, y0 - 1, x1 + 1, y1 + 1, 1.0, pal.ptr(hexColor("#000000")), 1.0);
        strokeRectAA(img, gx0, gy0, gx1, gy1, 2.0, pal.ptr(hexColor("#f5f5f5")), 1.0);
        // Corner ticks, the convention on a die drawing, and they survive the
        // palette quantisation that a GIF imposes better than a long thin line.
        const int tick = std::max(6, (gx1 - gx0) / 24);
        const std::uint8_t *c = pal.ptr(hexColor("#ffeb3b"));
        img.draw_rectangle(gx0, gy0, gx0 + tick, gy0, c, 1.0f, 1u);
        img.draw_rectangle(gx0, gy1 - 1, gx0 + tick, gy1, c, 1.0f, 1u);
        img.draw_rectangle(gx1 - tick, gy0, gx1, gy0, c, 1.0f, 1u);
        img.draw_rectangle(gx1 - tick, gy1 - 1, gx1, gy1, c, 1.0f, 1u);
    }

    // Fence regions, under the cells so the placement stays readable.
    if (constraints != nullptr) {
        static const char *kFenceColors[] = {"#ffb74d", "#ba68c8", "#4db6ac", "#f06292",
                                             "#9575cd", "#ffd54f", "#4fc3f7", "#a1887f"};
        for (std::size_t ri = 0; ri < constraints->numRegions(); ++ri) {
            const Region &reg = *constraints->region(static_cast<int>(ri));
            const char *hex = kFenceColors[ri % (sizeof(kFenceColors) / sizeof(char *))];
            const Rgb24 flat = blendOnBg(hexColor(hex), 0.13);
            const Rgb24 edge = blendOnBg(hexColor(hex), 0.9);
            for (const Rect &r : reg.rects) {
                int x0, y0, x1, y1;
                pixelSpan(toPxX(vp, r.lo.x), toPxX(vp, r.hi.x), x0, x1);
                pixelSpan(toPxY(vp, r.hi.y), toPxY(vp, r.lo.y), y0, y1);
                img.draw_rectangle(x0, y0, x1, y1, pal.ptr(flat), 1.0f);
                img.draw_rectangle(x0, y0, x1, y1, pal.ptr(edge), 1.0f, 1u);
            }
            img.draw_text(static_cast<int>(toPxX(vp, reg.minX)) + 3,
                          static_cast<int>(toPxY(vp, reg.minY)) - 3, reg.name.c_str(),
                          pal.ptr(edge), 0, 1.0f, &CImgList<unsigned char>::font(13));
        }
    }

    const CImgList<unsigned char> &font = CImgList<unsigned char>::font(13);
    const auto drawMovable = [&](const char *hex, double alpha, bool fenced) {
        const Rgb24 flat = blendOnBg(hexColor(hex), alpha);
        const std::uint8_t *c = pal.ptr(flat);
        const std::uint8_t *rim = pal.ptr(darken(flat, 0.45));
        for (std::size_t v = 0; v < nv; v += stride) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
                continue;
            }
            if ((vert.regionId != constraintMgr::kNoRegion) != fenced) {
                continue;
            }
            // Fractional edges, so a cell narrower than a pixel still shows up as
            // a faint tint instead of being rounded away or jumping to a whole
            // pixel. The 1.0 floor keeps a sub-pixel cell visible at all.
            // toPxY is inverted -- world +y is pixel -y -- so the cell's TOP edge
            // is the smaller pixel row. Getting that backwards makes every cell
            // an empty box and the frame comes out blank.
            const double x0 = toPxX(vp, x[v]);
            const double x1 = x0 + std::max(1.0, vert.width * vp.sx);
            const double yTop = toPxY(vp, y[v] + vert.height);
            const double yBot = toPxY(vp, y[v]);
            fillCellRect(img, x0, yTop, x1, yBot, c, rim);
        }
    };
    // I/O pads / terminals are decimated, there can be tens of thousands.
    {
        const Rgb24 flat = blendOnBg(hexColor("#ef5350"), 0.9);
        const std::uint8_t *c = pal.ptr(flat);
        const std::uint8_t *rim = pal.ptr(darken(flat, 0.45));
        const auto drawFixed = [&](std::size_t v) {
            const Vertex &vert = g.getVertex(v);
            const double x0 = toPxX(vp, x[v]);
            const double x1 = x0 + std::max(2.0, vert.width * vp.sx);
            const double yTop = toPxY(vp, y[v] + vert.height);
            const double yBot = toPxY(vp, y[v]);
            fillCellRect(img, x0, yTop, x1, yBot, c, rim);
        };
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type == VertexType::Cell && vert.isFixed) {
                drawFixed(v);
            }
        }
        for (std::size_t v = 0; v < nv; v += stride) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type == VertexType::Cell && vert.isTerminal && !vert.isFixed) {
                drawFixed(v);
            }
        }
    }

    // Nearly the pure hue rather than half-mixed into the background: the SVG now
    // draws these opaque, and a raster fill that is half background reads as a
    // transparency the vector frame does not have.
    drawMovable("#4fc3f7", 0.92, false);
    drawMovable("#00e676", 0.95, true);

    // Captions.
    const int tx = static_cast<int>(kMargin / 3);
    const std::string title =
        "CG step " + std::to_string(step) + " / " + std::to_string(numSteps - 1) + " - " + note;
    img.draw_text(tx, 16, title.c_str(), pal.ptr(hexColor("#ffffff")), 0, 1.0f, &font);

    const std::string wl = "HPWL = " + fmt(hpwl, 3) + "  (initial " + fmt(hpwlInitial, 3) + ")";
    img.draw_text(tx, 36, wl.c_str(), pal.ptr(hexColor("#90caf9")), 0, 1.0f, &font);

    int legendY = 56;
    if (resid > 0.0) {
        img.draw_text(tx, legendY, ("density overflow = " + sci(resid)).c_str(),
                      pal.ptr(hexColor("#ffeb3b")), 0, 1.0f, &font);
        legendY += 18;
    }

    // Legend: a colour swatch plus a label for each cell category.
    {
        struct Item {
            const char *hex;
            const char *label;
        };
        static const Item kItems[] = {
            {"#4fc3f7", "movable"}, {"#00e676", "fence-assigned"}, {"#ef5350", "fixed macro"}};
        int cx = tx;
        for (const Item &it : kItems) {
            const Rgb24 flat = blendOnBg(hexColor(it.hex), 0.9);
            img.draw_rectangle(cx, legendY + 2, cx + 10, legendY + 12, pal.ptr(flat), 1.0f);
            cx += 14;
            img.draw_text(cx, legendY, it.label, pal.ptr(hexColor("#90caf9")), 0, 1.0f, &font);
            cx += textWidth(font, it.label) + 12;
        }
    }

    return img;
}

/// Read one header integer, skipping whitespace and `#` comments.
int ppmNextInt(std::istream &in, const std::string &path) {
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
    w = ppmNextInt(in, path);
    h = ppmNextInt(in, path);
    const int maxVal = ppmNextInt(in, path);
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

}  // namespace

bool ensureDir(const std::string &dir) {
    if (dir.empty()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

BBox fixedCellBBox(const Graph &g) {
    // The "die" is taken as the bounding box of all fixed/terminal cells.
    // Vertices store their lower-left corner, so the box must also include
    // corner + (width, height): pads anchored at the right/top rim would
    // otherwise overhang the drawn die rectangle.
    const std::size_t nv = g.getNumVertices();
    double minX = std::numeric_limits<double>::max();
    double minY = std::numeric_limits<double>::max();
    double maxX = -std::numeric_limits<double>::max();
    double maxY = -std::numeric_limits<double>::max();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        if (!vert.isFixed && !vert.isTerminal) {
            continue;
        }
        minX = std::min(minX, vert.x);
        minY = std::min(minY, vert.y);
        maxX = std::max(maxX, vert.x + std::max(vert.width, 1.0));
        maxY = std::max(maxY, vert.y + std::max(vert.height, 1.0));
    }
    if (minX == std::numeric_limits<double>::max()) {
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
        double aMinX = std::numeric_limits<double>::max();
        double aMinY = std::numeric_limits<double>::max();
        double aMaxX = -std::numeric_limits<double>::max();
        double aMaxY = -std::numeric_limits<double>::max();
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            aMinX = std::min(aMinX, vert.x);
            aMinY = std::min(aMinY, vert.y);
            aMaxX = std::max(aMaxX, vert.x + std::max(vert.width, 1.0));
            aMaxY = std::max(aMaxY, vert.y + std::max(vert.height, 1.0));
        }
        if (aMinX == std::numeric_limits<double>::max()) {
            return {0.0, 0.0, 1.0, 1.0};  // genuinely empty graph
        }
        return {aMinX, aMinY, aMaxX, aMaxY};
    }
    return {minX, minY, maxX, maxY};
}

void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note, const constraintMgr *constraints, bool fixedView) {
    const std::size_t nv = g.getNumVertices();
    // By default the view auto-fits the data, which is right for a single frame but
    // makes a sequence impossible to read: a collapsed iteration 0 and a spread
    // iteration 9 are drawn at different scales, so the eye compares zoom levels
    // rather than placements. fixedView pins the viewport to the die so every
    // frame in a sequence is directly comparable.
    const ViewPort vp = fixedView ? dieViewPort(dieBox) : makeViewPort(g, x, y, dieBox);

    // Decimate so very large designs still produce small files.
    const std::size_t stride = (nv > kMaxPoints) ? ((nv + kMaxPoints - 1) / kMaxPoints) : 1;

    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << kImageW << "\" height=\""
        << kImageH << "\" viewBox=\"0 0 " << kImageW << " " << kImageH << "\">\n";
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
    out << "<rect x=\"" << kMargin / 3 << "\" y=\"" << kImageH - 14 << "\" width=\""
        << (kImageW - 2 * kMargin / 3) * pct / 100.0 << "\" height=\"6\" fill=\"#4fc3f7\"/>\n";

    // Die (fixed-pad) frame.
    out << "<rect x=\"" << fmt(toPxX(vp, dieBox[0])) << "\" y=\"" << fmt(toPxY(vp, dieBox[3]))
        << "\" width=\"" << fmt((dieBox[2] - dieBox[0]) * vp.sx) << "\" height=\""
        << fmt((dieBox[3] - dieBox[1]) * vp.sy)
        << "\" fill=\"none\" stroke=\"#bdbdbd\" stroke-width=\"1\"/>\n";

    // Fence regions, drawn under the cells so the placement stays readable.
    // Each region is a union of rectangles, so every piece is outlined and
    // filled; the name is labelled at the region's lower-left corner.
    if (constraints != nullptr) {
        static const char *kFenceColors[] = {"#ffb74d", "#ba68c8", "#4db6ac", "#f06292",
                                             "#9575cd", "#ffd54f", "#4fc3f7", "#a1887f"};
        for (std::size_t ri = 0; ri < constraints->numRegions(); ++ri) {
            const Region &reg = *constraints->region(static_cast<int>(ri));
            const char *color = kFenceColors[ri % (sizeof(kFenceColors) / sizeof(char *))];
            for (const Rect &r : reg.rects) {
                out << "<rect x=\"" << fmt(toPxX(vp, r.lo.x)) << "\" y=\"" << fmt(toPxY(vp, r.hi.y))
                    << "\" width=\"" << fmt((r.hi.x - r.lo.x) * vp.sx) << "\" height=\""
                    << fmt((r.hi.y - r.lo.y) * vp.sy) << "\" fill=\"" << color
                    << "\" fill-opacity=\"0.13\" stroke=\"" << color
                    << "\" stroke-width=\"1.5\" stroke-opacity=\"0.9\"/>\n";
            }
            out << "<text x=\"" << fmt(toPxX(vp, reg.minX) + 3) << "\" y=\""
                << fmt(toPxY(vp, reg.minY) - 3) << "\" fill=\"" << color
                << "\" font-family=\"monospace\" font-size=\"11\">" << reg.name << "</text>\n";
        }
    }

    // Fixed macros: never decimate, so hard cells match the DEF floorplan.
    out << "<g fill=\"#ef5350\" stroke=\"#000000\" stroke-opacity=\"0.55\""
        << " stroke-width=\"0.7\">\n";
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
            << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3) << "\" height=\""
            << fmt(h, 3) << "\"/>\n";
    }
    // I/O pads / terminals: decimatable, there can be tens of thousands.
    for (std::size_t v = 0; v < nv; v += stride) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isTerminal || vert.isFixed) {
            continue;
        }
        const double w = std::max(2.0, vert.width * vp.sx);
        const double h = std::max(2.0, vert.height * vp.sy);
        out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
            << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3) << "\" height=\""
            << fmt(h, 3) << "\"/>\n";
    }
    // Movable cells, split by whether the cell is tied to a placement region.
    // Fence-assigned cells are drawn opaque green over the ordinary blue, so a
    // glance shows whether a group's cells actually ended up in their fence.
    // Opaque, with a dark rim. The cells used to be drawn at 0.55 opacity, which
    // is what made a frame disagree with the GIF: the raster path cannot composite
    // per-pixel against an unknown backdrop, so it pre-mixed the colour toward the
    // background instead, and the two representations of the same frame came out
    // looking like different pictures. A solid fill plus a rim is unambiguous, and
    // the rim is what keeps abutting cells distinguishable in a legal placement.
    const auto drawMovable = [&](const char *color, bool fenced) {
        out << "<g fill=\"" << color << "\" stroke=\"#000000\" stroke-opacity=\"0.55\""
            << " stroke-width=\"0.7\">\n";
        for (std::size_t v = 0; v < nv; v += stride) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
                continue;
            }
            if ((vert.regionId != constraintMgr::kNoRegion) != fenced) {
                continue;
            }
            const double w = std::max(1.0, vert.width * vp.sx);
            const double h = std::max(1.0, vert.height * vp.sy);
            out << "<rect x=\"" << fmt(toPxX(vp, x[v])) << "\" y=\""
                << fmt(toPxY(vp, y[v] + vert.height)) << "\" width=\"" << fmt(w, 3)
                << "\" height=\"" << fmt(h, 3) << "\"/>\n";
        }
        out << "</g>\n";
    };
    drawMovable("#4fc3f7", false);
    drawMovable("#00e676", true);

    out << "</g>\n";

    out << "<text x=\"" << kMargin / 3
        << "\" y=\"24\" fill=\"#ffffff\" font-family=\"monospace\" font-size=\"14\">" << "CG step "
        << step << " / " << (numSteps - 1) << " — " << note << "</text>\n";
    out << "<text x=\"" << kMargin / 3
        << "\" y=\"44\" fill=\"#90caf9\" font-family=\"monospace\" font-size=\"13\">"
        << "HPWL = " << fmt(hpwl, 3) << "  (initial " << fmt(hpwlInitial, 3) << ")</text>\n";
    if (resid > 0.0) {
        out << "<text x=\"" << kMargin / 3
            << "\" y=\"62\" fill=\"#ffeb3b\" font-family=\"monospace\" font-size=\"12\">"
            << "density overflow = " << sci(resid) << "</text>\n";
    }
    // Legend for the cell colours.
    out << "<text x=\"" << kMargin / 3 << "\" y=\"" << (resid > 0.0 ? 80 : 62)
        << "\" fill=\"#90caf9\" font-family=\"monospace\" font-size=\"12\">"
        << "<tspan fill=\"#4fc3f7\">&#9632;</tspan> movable"
        << "   <tspan fill=\"#00e676\">&#9632;</tspan> fence-assigned"
        << "   <tspan fill=\"#ef5350\">&#9632;</tspan> fixed macro</text>\n";
    out << "</svg>\n";
    out.close();
}

void writeFrameRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                      const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                      std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                      const std::string &note, const constraintMgr *constraints, bool fixedView) {
    // A throwaway palette is fine for a single still; writeAnimatedGif() is the
    // path that needs one palette shared by every frame.
    GifPalette pal;
    const CImg<unsigned char> img =
        renderFrameCImg(g, x, y, dieBox, step, numSteps, hpwl, hpwlInitial, resid, note,
                        constraints, fixedView, pal);
    // Dispatch on the extension. ".ppm" and ".bmp" are handled natively by CImg
    // and need no external library; ".png" only works where libpng is installed.
    img.save(path.c_str());
}

namespace {
// The stills one stage contributed, in frame order. Names are zero-padded, so
// lexical order is frame order.
std::vector<std::string> collectFrames(const std::string &dir) {
    // Reading the frames back rather than buffering them keeps memory flat over
    // a long run, and the stills stay on disk as a browsable fallback. Only PPM
    // is accepted: it is the one format here that can be written and read back
    // correctly.
    static const char *kExts[] = {".ppm"};
    std::vector<std::string> frames;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec || !entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.rfind(kFramePrefix, 0) != 0) {
            continue;
        }
        for (const char *ext : kExts) {
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

bool writeAnimatedGif(const std::string &dir, const std::string &gifName, int delayCs) {
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
    constexpr std::size_t kMaxPalette = 256;
    // Sample every Nth pixel of every Nth frame. The palette only needs to
    // represent the distribution, not every pixel, and a full pass over 300 frames
    // of 768x768 is 176M pixels to look at.
    constexpr std::size_t kSampleStride = 5;
    constexpr std::size_t kFrameStride = 3;

    std::map<std::uint32_t, std::uint32_t> histogram;  // packed rgb -> occurrences
    struct Frame {
        int w = 0, h = 0;
        std::vector<std::uint8_t> idx;
        std::vector<std::uint8_t> rgb;
    };
    std::vector<Frame> loaded;
    loaded.reserve(frames.size());
    for (std::size_t fi = 0; fi < frames.size(); ++fi) {
        Frame f;
        std::vector<std::uint8_t> rgb;
        try {
            loadPpm6(frames[fi], f.w, f.h, rgb);
        } catch (const std::exception &) {
            continue;  // unreadable or truncated still; skip it
        }
        f.rgb = rgb;
        if (fi % kFrameStride == 0) {
            const std::size_t np = rgb.size() / 3;
            for (std::size_t i = 0; i < np; i += kSampleStride) {
                ++histogram[packRgb(rgb[3 * i], rgb[3 * i + 1], rgb[3 * i + 2])];
            }
        }
        loaded.push_back(std::move(f));
    }
    if (loaded.empty()) {
        return false;
    }

    GifPalette pal;
    for (const Rgb24 &c : medianCutPalette(histogram, kMaxPalette)) {
        pal.index(c);
    }
    // Any colour the sample missed still needs an entry, or it would be mapped to
    // whatever happened to be nearest. Appending the remainder keeps the mapping
    // exact where it can be and only approximate at the long tail.
    if (pal.colors().size() < kMaxPalette) {
        for (const auto &[key, count] : histogram) {
            (void)count;
            pal.index(Rgb24{static_cast<std::uint8_t>((key >> 16) & 0xff),
                            static_cast<std::uint8_t>((key >> 8) & 0xff),
                            static_cast<std::uint8_t>(key & 0xff)});
            if (pal.colors().size() >= kMaxPalette) {
                break;
            }
        }
    }

    std::vector<Frame> quantised;
    quantised.reserve(loaded.size());
    for (Frame &f : loaded) {
        const std::size_t np = f.rgb.size() / 3;
        f.idx.resize(np);
        for (std::size_t i = 0; i < np; ++i) {
            f.idx[i] = pal.index(Rgb24{f.rgb[3 * i], f.rgb[3 * i + 1], f.rgb[3 * i + 2]});
        }
        f.rgb.clear();
        f.rgb.shrink_to_fit();
        quantised.push_back(std::move(f));
    }

    std::vector<Rgb> palRgb;
    palRgb.reserve(pal.colors().size());
    for (const Rgb24 &c : pal.colors()) {
        palRgb.push_back(Rgb{c.r, c.g, c.b});
    }

    std::vector<Canvas> canvases;
    canvases.reserve(quantised.size());
    for (const Frame &f : quantised) {
        canvases.emplace_back(f.w, f.h, f.idx, palRgb);
    }

    const std::string gifPath = (std::filesystem::path(dir) / gifName).string();
    if (!writeGif(gifPath, canvases, delayCs)) {
        return false;
    }
    return true;
}

void writeHpwlCurve(const std::string &csvPath, const std::string &svgPath,
                    const std::vector<std::pair<std::size_t, double>> &curve,
                    const std::vector<double> *residuals) {
    if (curve.empty()) {
        return;
    }

    {
        std::ofstream out(csvPath);
        if (out.is_open()) {
            out << "step,hpwl,overflow\n";
            for (std::size_t i = 0; i < curve.size(); ++i) {
                out << curve[i].first << "," << std::setprecision(10) << curve[i].second;
                if (residuals && residuals->size() == curve.size() && (*residuals)[i] > 0.0) {
                    out << "," << std::setprecision(6) << std::scientific << (*residuals)[i];
                } else {
                    out << ",";
                }
                out << "\n";
            }
            out.close();
        }
    }

    const double cw = 900.0, ch = 300.0, l = 60.0, r = 20.0, t = 30.0, b = 40.0;
    const double maxStep = static_cast<double>(curve.back().first);
    double hpwlMin = std::numeric_limits<double>::max();
    double hpwlMax = -std::numeric_limits<double>::max();
    for (const auto &[s, h] : curve) {
        hpwlMin = std::min(hpwlMin, h);
        hpwlMax = std::max(hpwlMax, h);
    }
    const double hpwlSpan = std::max(hpwlMax - hpwlMin, 1e-9);

    // Residual range (optional secondary curve, normalized to its own span).
    double rMin = 0.0, rMax = 0.0;
    bool hasResid = residuals && residuals->size() == curve.size();
    if (hasResid) {
        rMin = std::numeric_limits<double>::max();
        rMax = -std::numeric_limits<double>::max();
        for (double rv : *residuals) {
            if (rv <= 0.0)
                continue;
            rMin = std::min(rMin, rv);
            rMax = std::max(rMax, rv);
        }
        if (rMin > rMax) {
            hasResid = false;
        }
    }
    const double rSpan = hasResid ? std::max(rMax - rMin, 1e-300) : 1.0;

    std::ofstream out(svgPath);
    if (!out.is_open()) {
        return;
    }
    auto px = [&](double s) {
        return l + (s / std::max(maxStep, 1.0)) * (cw - l - r);
    };
    auto py = [&](double h) {
        return t + (1.0 - (h - hpwlMin) / hpwlSpan) * (ch - t - b);
    };
    auto pry = [&](double rv) {
        return t + (1.0 - (rv - rMin) / rSpan) * (ch - t - b);
    };

    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << cw << "\" height=\"" << ch
        << "\" viewBox=\"0 0 " << cw << " " << ch << "\">\n";
    out << "<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<line x1=\"" << l << "\" y1=\"" << t << "\" x2=\"" << l << "\" y2=\"" << ch - b
        << "\" stroke=\"#455\" stroke-width=\"1\"/>\n";
    out << "<line x1=\"" << l << "\" y1=\"" << ch - b << "\" x2=\"" << cw - r << "\" y2=\""
        << ch - b << "\" stroke=\"#455\" stroke-width=\"1\"/>\n";

    // Baseline: initial HPWL.
    out << "<line x1=\"" << px(0.0) << "\" y1=\"" << py(curve.front().second) << "\" x2=\""
        << px(maxStep) << "\" y2=\"" << py(curve.front().second)
        << "\" stroke=\"#ef5350\" stroke-width=\"1\" stroke-dasharray=\"4,4\"/>\n";

    out << "<polyline fill=\"none\" stroke=\"#4fc3f7\" stroke-width=\"2\" points=\"";
    for (const auto &[s, h] : curve) {
        out << fmt(px(static_cast<double>(s)), 2) << "," << fmt(py(h), 2) << " ";
    }
    out << "\"/>\n";

    if (hasResid) {
        out << "<polyline fill=\"none\" stroke=\"#ffeb3b\" stroke-width=\"1.5\" "
               "stroke-dasharray=\"2,2\" points=\"";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            if ((*residuals)[i] <= 0.0)
                continue;
            out << fmt(px(static_cast<double>(curve[i].first)), 2) << ","
                << fmt(pry((*residuals)[i]), 2) << " ";
        }
        out << "\"/>\n";
    }

    for (const auto &[s, h] : curve) {
        out << "<circle cx=\"" << fmt(px(static_cast<double>(s)), 2) << "\" cy=\"" << fmt(py(h), 2)
            << "\" r=\"2.5\" fill=\"" << (h <= hpwlMin + 0.01 * hpwlSpan ? "#ffeb3b" : "#4fc3f7")
            << "\"/>\n";
    }

    out << "<text x=\"" << cw / 2 - 40
        << "\" y=\"16\" fill=\"#ffffff\" font-family=\"monospace\" font-size=\"13\">"
        << "HPWL (blue) vs outer step" << (hasResid ? "  +  density overflow (yellow)" : "")
        << "</text>\n";
    if (hasResid) {
        out << "<text x=\"" << cw / 2 - 40
            << "\" y=\"30\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\">"
            << "residual " << sci(rMax) << " -> " << sci(rMin) << "</text>\n";
    }
    out << "<text x=\"" << l - 8 << "\" y=\"" << ch - b + 18
        << "\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\" text-anchor=\"end\">"
        << fmt(hpwlMax, 3) << " max</text>\n";
    out << "<text x=\"" << l << "\" y=\"" << py(hpwlMax) + 16
        << "\" fill=\"#9aa\" font-family=\"monospace\" font-size=\"11\">" << fmt(hpwlMin, 3)
        << " min</text>\n";
    out << "</svg>\n";
    out.close();
}

void writeGallery(const std::string &dir, const std::vector<std::string> &framePaths,
                  const std::string &csvName) {
    std::ofstream out((std::filesystem::path(dir) / "index.html").string());
    if (!out.is_open()) {
        return;
    }
    out << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
        << "<title>KTPlace placement visualization</title>\n"
        << "<style>\n"
        << "body{font-family:sans-serif;margin:2em;background:#0b0f12;color:#ddd;}\n"
        << "h1{color:#fff;}\n"
        << ".curve img{max-width:920px;border:1px solid #333;}\n"
        << ".frame{display:inline-block;margin:6px;}\n"
        << ".frame img{max-width:360px;border:1px solid #333;background:#101418;}\n"
        << ".frame div{font-size:12px;color:#9aa;}\n"
        << "</style>\n</head>\n<body>\n"
        << "<h1>KTPlace placement visualization</h1>\n"
        << "<p>Snapshots taken during global placement. Movable cells in blue, fixed pads in red, "
        << "dashed line = initial HPWL baseline. The yellow dashed curve is the density overflow; "
        << "it dropping toward zero means cells spread across the die instead of piling up. "
        << "Green hulls / pin-stars are a sampled net overlay: each rect is a chosen net's "
        << "bounding box; the star lines join its pins to the net centroid, so long-span "
        << "signals read at a glance. "
        << "HPWL may rise at first as the spreading force opens up collapsed regions — that is "
        << "expected for global placement (see README).</p>\n";
    out << "<div class=\"curve\"><img src=\"hpwl.svg\" alt=\"HPWL curve\"></div>\n"
        << "<p>Raw data: <a href=\"" << csvName << "\">" << csvName << "</a></p>\n";
    for (const auto &f : framePaths) {
        out << "<div class=\"frame\"><img src=\"" << f << "\"><div>" << f << "</div></div>\n";
    }
    out << "</body>\n</html>\n";
    out.close();
}

}  // namespace ktplace
