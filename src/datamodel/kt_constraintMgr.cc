// @file kt_constraintMgr.cc// Implementation of the placement region constraints


#include "datamodel/kt_constraintMgr.h"

#include <algorithm>
#include <limits>

namespace ktplace {

int constraintMgr::addRegion(std::string name, const std::vector<Point> &points) {
    Region region;
    region.name = std::move(name);

    // The fence is a flat list of corners taken two at a time: lower-left then
    // upper-right of one rectangle. A trailing odd corner cannot form a
    // rectangle and is dropped.
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        const BBox r(
            Point(std::min(points[i].x, points[i + 1].x), std::min(points[i].y, points[i + 1].y)),
            Point(std::max(points[i].x, points[i + 1].x), std::max(points[i].y, points[i + 1].y)));
        if (!(r.getArea() > 0.0)) {
            continue;  // degenerate rectangle
        }
        region.rects.push_back(r);
    }
    if (region.rects.empty()) {
        return noRegion;  // unusable as a fence
    }

    region.area = 0.0;
    for (const BBox &r : region.rects) {
        region.area += r.getArea();
    }
    region.bounds = BBox::getEmpty();
    for (const BBox &r : region.rects) {
        region.bounds.extend(r);
    }

    const int id = static_cast<int>(regionList.size());
    idByName[region.name] = id;
    regionList.push_back(std::move(region));
    return id;
}

namespace {

constexpr double areaTolerance = 1e-6;

double getOverlapArea(const BBox &a, const BBox &b) {
    const BBox overlap = a.getIntersection(b);
    const double width = overlap.getWidth();
    const double height = overlap.getHeight();
    return width > 0.0 && height > 0.0 ? width * height : 0.0;
}

}  // namespace

bool constraintMgr::containsBox(int id, const BBox &box) const {
    const Region *r = getRegion(id);
    if (r == nullptr) {
        return false;
    }
    double covered = 0.0;
    for (const BBox &rect : r->rects) {
        covered += getOverlapArea(rect, box);
    }
    return covered >= box.getArea() * (1.0 - areaTolerance);
}

bool constraintMgr::overlapsAnyRegion(const BBox &box) const {
    return std::ranges::any_of(regionList, [&](const Region &r) {
        return std::ranges::any_of(r.rects, [&](const BBox &rect) {
            return getOverlapArea(rect, box) > areaTolerance * box.getArea();
        });
    });
}

bool constraintMgr::contains(int id, const Point &p) const {
    if (id == noRegion) {
        return true;  // unconstrained cells are always legal
    }
    const Region *r = getRegion(id);
    return r != nullptr && r->contains(p);
}

void constraintMgr::clampToRegion(int id, Point &p) const {
    const Region *r = getRegion(id);
    if (r == nullptr || r->contains(p)) {
        return;
    }
    // Snap into the rectangle that is nearest, measured as the distance still
    // to travel along each axis.
    const BBox *best = nullptr;
    double bestDist = std::numeric_limits<double>::max();
    for (const BBox &cand : r->rects) {
        const double dx = cand.getX().getClamped(p.x) - p.x;
        const double dy = cand.getY().getClamped(p.y) - p.y;
        const double dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = &cand;
        }
    }
    if (best == nullptr) {
        return;
    }
    p = Point(best->getX().getClamped(p.x), best->getY().getClamped(p.y));
}

double constraintMgr::getEscapeEpsilon() const {
    double scale = 0.0;
    for (const Region &r : regionList) {
        scale = std::max(scale, std::max(r.bounds.getWidth(), r.bounds.getHeight()));
    }
    return std::max(scale, 1.0) * 1e-6;
}

bool constraintMgr::isInsideAnyRegion(const Point &p) const {
    for (const Region &r : regionList) {
        if (r.contains(p)) {
            return true;
        }
    }
    return false;
}

bool constraintMgr::pushOutOfRegions(Point &p, const BBox &die) const {
    // Escaping has to be planned against the *union* of the fences, not one
    // rectangle at a time. The ISPD fences are often rings or combs of
    // abutting rectangles, so stepping to the nearest edge of the rectangle that
    // caught the point can drop it straight into the neighbour and ping-pong
    // forever. Each pass therefore picks the nearest of the four exits that
    // actually lands outside every region, which makes progress monotone.
    const std::size_t maxPasses = 4 * (regionList.size() + 1) + 8;
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        const BBox *host = nullptr;
        double hostArea = std::numeric_limits<double>::max();
        for (const Region &r : regionList) {
            for (const BBox &cand : r.rects) {
                if (cand.contains(p) && cand.getArea() < hostArea) {
                    hostArea = cand.getArea();
                    host = &cand;
                }
            }
        }
        if (host == nullptr) {
            return pass > 0;  // clear of every fence
        }

        // The step has to be big enough to stay outside the fence once the
        // placement is written out and read back, so it is scaled to the
        // geometry rather than to the rectangle that caught the point: a
        // per-rectangle epsilon can be far below the output resolution, and the
        // cell then lands back exactly on the (inclusive) boundary.
        const double eps = getEscapeEpsilon();
        const Point exits[4] = {
            Point(host->lowerLeft.x - eps, p.y), Point(host->upperRight.x + eps, p.y),
            Point(p.x, host->lowerLeft.y - eps), Point(p.x, host->upperRight.y + eps)};
        const double dists[4] = {p.x - host->lowerLeft.x, host->upperRight.x - p.x,
                                 p.y - host->lowerLeft.y, host->upperRight.y - p.y};

        int best = -1;
        double bestDist = std::numeric_limits<double>::max();
        bool bestLegal = false;
        for (int k = 0; k < 4; ++k) {
            const bool legal = die.contains(exits[k]) && !isInsideAnyRegion(exits[k]);
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
        p = exits[best];
    }
    return true;
}

std::size_t constraintMgr::assignByPrefix(int regionId, const std::string &prefix, Graph &graph) {
    if (regionId == noRegion || getRegion(regionId) == nullptr || prefix.empty()) {
        return 0;
    }
    std::size_t assigned = 0;
    const std::size_t count = graph.getNumCells();
    for (std::size_t v = 0; v < count; ++v) {
        Vertex &vert = graph.getCell(v);
        if (vert.regionId != noRegion) {
            continue;  // first matching group wins
        }
        if (vert.name.compare(0, prefix.size(), prefix) == 0) {
            vert.regionId = regionId;
            ++assigned;
        }
    }
    if (regionId < static_cast<int>(regionList.size())) {
        regionList[regionId].cellCount = assigned;
    }
    return assigned;
}

std::size_t constraintMgr::countViolations(const std::vector<Point> &positions,
                                           const std::vector<int> &regionIds) const {
    std::size_t violations = 0;
    const std::size_t count = std::min(positions.size(), regionIds.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!contains(regionIds[i], positions[i])) {
            ++violations;
        }
    }
    return violations;
}

}  // namespace ktplace
