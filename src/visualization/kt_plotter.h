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
 *
 * Every cell in the graph is drawn, with no decimation.
 */
void writeFrameSvg(const std::string &path, const Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note, const constraintMgr *constraints = nullptr,
                   bool fixedView = false);

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
 * The output format follows the extension of @p path, which is dispatched on:
 * ".ppm" and ".bmp" are written by CImg and need no external library, and
 * ".png" is written by this codebase on top of zlib, which it already links.
 * ".ppm" is the one to use for the animation: it is a binary P6 file, can be
 * read back losslessly -- unlike ".bmp" and ".png", which CImg writes but
 * cannot reliably load again here -- and frames named "frame_NNNN.ppm" are what
 * writeAnimatedGif() collects, so a run can stream stills to disk and only pay
 * for the animation once, at the end.
 *
 * Every cell in the graph is drawn. There is no decimation: a frame that shows
 * some of the placement reads as the placer having lost the rest, and no design
 * is large enough for drawing all of its cells to be the expensive part.
 *
 * @param path      output image file
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
 * @param zoom      linear scale of the raster, vs the 768x768 frame size; the
 *                   drawing, the caption and the die frame all scale with it
 */
void writeFrameRaster(const std::string &path, const Graph &g, const std::vector<float> &x,
                      const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                      std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                      const std::string &note, const constraintMgr *constraints = nullptr,
                      bool fixedView = false, double zoom = 1.0);

/**
 * @brief Render the final placement as a high-resolution raster still.
 *
 * A single static picture of the finished placement at @p zoom times the frame
 * resolution (768x768 at zoom 1), intended as the "look at the result" image
 * after a run: the animation frames are small by design, because a GIF has to
 * be, and a big design's cells collapse to a pixel or two in them. At zoom 4
 * the same drawing is 3072x3072 and every cell resolves to a few pixels with
 * its rim visible.
 *
 * Every cell is drawn, as in every other frame here. The caption reads "final
 * placement" and carries no health metrics: it is the end of the run, not a CG
 * step.
 *
 * The output format follows the extension of @p path, exactly as in
 * writeFrameRaster(). Use ".png" for this one: the file is meant to be looked
 * at, and no image viewer opens a ".ppm".
 *
 * @param path      output image file
 * @param g         graph (for cell dimensions, names, fixed flag); the vertices
 *                  are expected to already hold the finished placement
 * @param constraints  placement regions to draw as fences; may be null
 * @param zoom      linear scale factor vs the 768x768 frame resolution
 * @param hpwl      the run's final wirelength, for the caption
 */
void writeFinalFrameRaster(const std::string &path, const Graph &g,
                           const constraintMgr *constraints = nullptr, double zoom = 4.0,
                           double hpwl = 0.0);

/**
 * @brief Write the finished placement as a vector SVG.
 *
 * The same picture as writeFinalFrameRaster() and in the same place: one image
 * per run, of the placement that is actually being written out. The vector form
 * is the one to open when a region has to be looked at closely, since it stays
 * sharp at any magnification, and it is also the exact record of the drawing --
 * one <rect> per cell, so "every cell is in the picture" is a count rather than
 * an estimate.
 *
 * @param path      output .svg file
 * @param g         graph holding the finished placement
 * @param constraints  placement regions to draw as fences; may be null
 * @param hpwl      the run's final wirelength, for the caption
 */
void writeFinalFrameSvg(const std::string &path, const Graph &g,
                        const constraintMgr *constraints = nullptr, double hpwl = 0.0);

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
 * buffered, so memory stays flat however many iterations there are.
 *
 * Once the GIF is written the stills are deleted: they are the encoder's
 * scratch space, and a long run leaves several hundred megabytes of P6 files
 * that nothing can open. If the encode fails they are left alone, since they
 * are then the only record of the run's frames.
 *
 * @param dir       directory holding the stills
 * @param gifName   output file name, created inside @p dir
 * @param delayCs   delay between frames in hundredths of a second
 * @return true if at least one frame was found and the GIF was written.
 */
bool writeAnimatedGif(const std::string &dir, const std::string &gifName, int delayCs = 60);

}  // namespace ktplace
