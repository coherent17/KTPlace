/**
 * @file kt_plotter.h
 * @brief Lightweight SVG/HTML/CSV visualization for placement snapshots
 *
 * Emits vector images of the die with cells drawn as dots, plus an HPWL-vs-step
 * curve, all as plain-text files viewable in any browser. No image
 * libraries required.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"

namespace ktplace {

/// Bounding box of a region: {xmin, ymin, xmax, ymax}.
using BBox = std::array<double, 4>;

/**
 * @brief Create a directory (and parents) if it does not exist.
 * @return true on success or if it already exists.
 */
bool ensureDir(const std::string &dir);

/**
 * @brief Bounding box of the fixed/terminal cells (the die frame).
 *        Returns {0,0,1,1} if there are no fixed cells.
 */
BBox fixedCellBBox(const Graph &g);

/**
 * @brief Render one placement snapshot as an SVG image.
 *
 * @param path      output .svg file
 * @param g         graph (for cell dimensions, names, fixed flag)
 * @param x         x coordinate per vertex id (movable overridden)
 * @param y         y coordinate per vertex id
 * @param dieBox    fixed-cell bounding box used to draw the die outline
 * @param step      current outer (WL+density) step (0 = initial placement)
 * @param numSteps  total steps (for a progress bar)
 * @param hpwl      half-perimeter wirelength at this snapshot
 * @param hpwlInitial  HPWL of the initial placement
 * @param resid     density overflow at this snapshot
 * @param note      human-readable label for the frame
 * @param constraints  placement regions to draw as fences; may be null
 * @param fixedView  ignore the current solution and draw the cells at their
 *                   original positions, so a series of frames shares one scale
 */
void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note, const constraintMgr *constraints = nullptr,
                   bool fixedView = false);

/**
 * @brief Write the HPWL-vs-CG-step curve as CSV plus an SVG line chart.
 * @param csvPath  output .csv file
 * @param svgPath  output .svg chart
 * @param curve  (step, hpwl) pairs in ascending step order.
 * @param residuals  optional per-step density overflow; if non-null and the
 *                   same size as curve, a third CSV column and a second
 *                   (yellow) curve are added.
 */
void writeHpwlCurve(const std::string &csvPath, const std::string &svgPath,
                    const std::vector<std::pair<std::size_t, double>> &curve,
                    const std::vector<double> *residuals = nullptr);

/**
 * @brief Write an HTML gallery page that embeds all frames and the curve.
 */
void writeGallery(const std::string &dir, const std::vector<std::string> &framePaths,
                  const std::string &csvName);

/**
 * @brief Render one placement snapshot as a raster image, the CImg
 *        counterpart of writeFrameSvg().
 *
 * Draws the same layout as writeFrameSvg() using CImg instead of SVG markup.
 * The output format follows the extension of @p path, which CImg dispatches
 * on. Use ".ppm": it is a binary P6 file, needs no external library, and can
 * be read back losslessly -- unlike ".bmp" and ".png", which CImg writes but
 * cannot reliably load again here, and ".png" additionally needs libpng
 * headers, which are not installed. Frames named "frame_NNNN.ppm" are what
 * writeAnimatedGif() collects, so a run can stream stills to disk and only pay
 * for the animation once, at the end.
 *
 * @param path      output image file; ".ppm" needs no external library
 * @param g         graph (for cell dimensions, names, fixed flag)
 * @param x         x coordinate per vertex id (movable overridden)
 * @param y         y coordinate per vertex id
 * @param dieBox    fixed-cell bounding box used to draw the die outline
 * @param step      current outer (WL+density) step (0 = initial placement)
 * @param numSteps  total steps (for a progress bar)
 * @param hpwl      half-perimeter wirelength at this snapshot
 * @param hpwlInitial  HPWL of the initial placement
 * @param resid     density overflow at this snapshot
 * @param note      human-readable label for the frame
 * @param constraints  placement regions to draw as fences; may be null
 * @param fixedView  draw the cells at their original positions, so a series of
 *                   frames shares one scale
 */
void writeFrameRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                      const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                      std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                      const std::string &note, const constraintMgr *constraints = nullptr,
                      bool fixedView = false);

/**
 * @brief Assemble the per-iteration raster stills in @p dir into one animated
 *        GIF.
 *
 * Self-contained: the GIF89a container, its LZW compression and the palette are
 * all written in-process, so no external tool (ffmpeg, ImageMagick) and no
 * external image library is involved -- the result is reproducible from the
 * ktplace binary alone. All frames share a single palette, which GIF requires
 * for colours to stay stable across the loop.
 *
 * Collects "frame_NNNN.ppm", which is what writeFrameRaster() produces under the
 * frame naming convention. The stills are read back from disk rather than
 * buffered, so memory stays flat however many iterations there are, and they
 * remain on afterwards as a browsable per-iteration record.
 *
 * @param dir       directory holding the stills
 * @param gifName   output file name, created inside @p dir
 * @param delayCs   delay between frames in hundredths of a second
 * @return true if at least one frame was found and the GIF was written.
 */
bool writeAnimatedGif(const std::string &dir, const std::string &gifName, int delayCs = 60);

}  // namespace ktplace
