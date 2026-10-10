#include "placer/simpl/simpl_fence.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace ktplace::simpl {

void Region::seal() {
    if (rects.empty()) {
        minX = minY = maxX = maxY = 0.0;
        return;
    }
    minX = minY = std::numeric_limits<double>::max();
    maxX = maxY = std::numeric_limits<double>::lowest();
    for (const Rect &r : rects) {
        minX = std::min(minX, r.xlo);
        minY = std::min(minY, r.ylo);
        maxX = std::max(maxX, r.xhi);
        maxY = std::max(maxY, r.yhi);
    }
}

bool Fences::clampToRegion(int id, double &x, double &y) const {
    if (id < 0 || static_cast<std::size_t>(id) >= regions.size()) {
        return false;
    }
    const Region &r = regions[static_cast<std::size_t>(id)];
    if (r.contains(x, y)) {
        return false;
    }
    // Snap into the rectangle that is nearest, measured as the distance still
    // to travel along each axis.
    const Rect *best = nullptr;
    double bestDist = std::numeric_limits<double>::max();
    for (const Rect &cand : r.rects) {
        const double cx = std::clamp(x, cand.xlo, cand.xhi);
        const double cy = std::clamp(y, cand.ylo, cand.yhi);
        const double dx = cx - x;
        const double dy = cy - y;
        const double dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = &cand;
        }
    }
    if (best == nullptr) {
        return false;
    }
    x = std::clamp(x, best->xlo, best->xhi);
    y = std::clamp(y, best->ylo, best->yhi);
    return true;
}

namespace {

bool insideAnyRegion(const std::vector<Region> &regions, double x, double y) {
    for (const Region &r : regions) {
        if (r.contains(x, y)) {
            return true;
        }
    }
    return false;
}

double escapeEpsilon(const std::vector<Region> &regions) {
    double scale = 0.0;
    for (const Region &r : regions) {
        scale = std::max(scale, std::max(r.maxX - r.minX, r.maxY - r.minY));
    }
    return std::max(scale, 1.0) * 1e-6;
}

}  // namespace

bool Fences::pushOutOfRegions(double &x, double &y, double dieMinX, double dieMinY, double dieMaxX,
                              double dieMaxY) const {
    // Escaping is planned against the *union* of the fences, not one rectangle
    // at a time. Fences are often rings or combs of abutting rectangles, so
    // stepping to the nearest edge of the rectangle that caught the point can
    // drop it into the neighbour and ping-pong forever. Each pass therefore picks
    // the nearest of the four exits that actually lands outside every region,
    // which makes progress monotone.
    const std::size_t maxPasses = 4 * (regions.size() + 1) + 8;
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        const Rect *host = nullptr;
        double hostArea = std::numeric_limits<double>::max();
        for (const Region &r : regions) {
            for (const Rect &cand : r.rects) {
                const double area = cand.width() * cand.height();
                if (cand.contains(x, y) && area < hostArea) {
                    hostArea = area;
                    host = &cand;
                }
            }
        }
        if (host == nullptr) {
            return pass > 0;  // clear of every fence
        }

        // The step has to survive the placement being written out and read back,
        // so it is scaled to the overall geometry rather than to the rectangle
        // that caught the point: a per-rectangle epsilon can fall below the
        // output resolution, and the cell then lands back on the boundary.
        const double eps = escapeEpsilon(regions);
        const double exits[4][2] = {
            {host->xlo - eps, y}, {host->xhi + eps, y}, {x, host->ylo - eps}, {x, host->yhi + eps}};
        const double dists[4] = {x - host->xlo, host->xhi - x, y - host->ylo, host->yhi - y};

        int best = -1;
        double bestDist = std::numeric_limits<double>::max();
        bool bestLegal = false;
        for (int k = 0; k < 4; ++k) {
            const bool onDie = exits[k][0] >= dieMinX && exits[k][0] <= dieMaxX &&
                               exits[k][1] >= dieMinY && exits[k][1] <= dieMaxY;
            const bool legal = onDie && !insideAnyRegion(regions, exits[k][0], exits[k][1]);
            if (legal && (!bestLegal || dists[k] < bestDist)) {
                best = k;
                bestDist = dists[k];
                bestLegal = true;
            }
        }
        if (best < 0) {
            // No side leads to legal die area (every side abuts another fence, or
            // the rest of the fence is off-die): take the shortest way out and
            // let the next pass continue from there.
            best = static_cast<int>(std::min_element(dists, dists + 4) - dists);
        }
        x = exits[best][0];
        y = exits[best][1];
    }
    return true;
}

std::size_t Fences::countViolations(const std::vector<double> &positions,
                                    const std::vector<int> &regionIds) const {
    std::size_t bad = 0;
    const std::size_t n = std::min(regionIds.size(), positions.size() / 2);
    for (std::size_t i = 0; i < n; ++i) {
        const int id = regionIds[i];
        if (id < 0 || static_cast<std::size_t>(id) >= regions.size()) {
            continue;
        }
        if (!regions[static_cast<std::size_t>(id)].contains(positions[2 * i],
                                                            positions[2 * i + 1])) {
            ++bad;
        }
    }
    return bad;
}

}  // namespace ktplace::simpl
