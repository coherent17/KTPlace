/**
 * @file kt_plotter.h
 * @brief Lightweight SVG/HTML/CSV visualization for placement snapshots
 *
 * Emits vector images of the die with cells drawn as dots, plus an HPWL-vs-step
 * curve, all as plain-text files viewable in any browser. No image
 * libraries required.
 */

#ifndef KT_PLOTTER_H
#define KT_PLOTTER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "datamodel/kt_graph.h"

namespace ktplace {
namespace io {

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
BBox fixedCellBBox(const core::Graph &g);

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
 */
void writeFrameSvg(const std::string &path, const core::Graph &g, const std::vector<float> &x,
                   const std::vector<float> &y, const BBox &dieBox, std::size_t step,
                   std::size_t numSteps, double hpwl, double hpwlInitial, double resid,
                   const std::string &note);

/**
 * @brief Write the HPWL-vs-CG-step curve as CSV plus an SVG line chart.
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

}  // namespace io
}  // namespace ktplace

#endif  // KT_PLOTTER_H