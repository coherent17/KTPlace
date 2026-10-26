#include "globalPlacer/simpl/simpl_dm.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "globalPlacer/simpl/simpl_placer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>

namespace ktplace::simpl {
namespace {

void collectCells(const ktplace::Graph &graph, Design &design) {
    Netlist &netlist = design.netlist;
    const std::size_t numCells = graph.getNumCells();
    netlist.varOfVertex.assign(numCells, noVar);
    netlist.fixedX.assign(numCells, 0.0);
    netlist.fixedY.assign(numCells, 0.0);
    for (std::size_t v = 0; v < numCells; ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        netlist.fixedX[v] = cell.x;
        netlist.fixedY[v] = cell.y;
        if (cell.isFixed || cell.isTerminal) {
            if (cell.width > 0.0 && cell.height > 0.0) {
                design.obstacles.push_back(
                    {.lowerLeft = {.x = cell.x, .y = cell.y},
                     .upperRight = {.x = cell.x + cell.width, .y = cell.y + cell.height}});
            }
            ++design.numFixed;
            continue;
        }
        netlist.varOfVertex[v] = static_cast<std::uint32_t>(netlist.numMovable++);
        netlist.movVertex.push_back(static_cast<std::uint32_t>(v));
        design.width.push_back(cell.width);
        design.height.push_back(cell.height);
        design.area.push_back(cell.width * cell.height);
        design.region.push_back(cell.regionId);
        design.x.push_back(cell.x);
        design.y.push_back(cell.y);
    }
}

void collectNets(const ktplace::Graph &graph, Netlist &netlist) {
    netlist.nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < netlist.nets.size(); ++n) {
        const std::vector<std::size_t> &pins = graph.getNetPins(n);
        std::vector<std::size_t> order(pins.begin(), pins.end());
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return graph.getPin(a).cellId < graph.getPin(b).cellId;
        });
        NetPins &net = netlist.nets[n];
        net.weight = graph.getNet(n).weight;
        for (const std::size_t pinId : order) {
            const ktplace::Pin &pin = graph.getPin(pinId);
            if (!net.cells.empty() && net.cells.back() == pin.cellId) {
                continue;
            }
            net.cells.push_back(static_cast<std::uint32_t>(pin.cellId));
            net.offX.push_back(pin.offsetX);
            net.offY.push_back(pin.offsetY);
        }
    }

    netlist.pinStart.assign(netlist.numMovable + 1, 0);
    for (const NetPins &net : netlist.nets) {
        for (const std::uint32_t c : net.cells) {
            if (netlist.varOfVertex[c] != noVar) {
                ++netlist.pinStart[netlist.varOfVertex[c] + 1];
            }
        }
    }
    std::partial_sum(netlist.pinStart.begin(), netlist.pinStart.end(), netlist.pinStart.begin());
    netlist.pinsOfCell.resize(netlist.pinStart.back());
    std::vector<std::size_t> cursor(netlist.pinStart.begin(), netlist.pinStart.end() - 1);
    for (std::uint32_t n = 0; n < netlist.nets.size(); ++n) {
        for (std::uint32_t q = 0; q < netlist.nets[n].cells.size(); ++q) {
            const std::uint32_t v = netlist.varOfVertex[netlist.nets[n].cells[q]];
            if (v != noVar) {
                netlist.pinsOfCell[cursor[v]++] = {.net = n, .pin = q};
            }
        }
    }
}

std::vector<RowInfo> copyRows(const std::vector<ktplace::RowInfo> &rows) {
    std::vector<RowInfo> out;
    out.reserve(rows.size());
    for (const ktplace::RowInfo &row : rows) {
        RowInfo info{.coordinate = row.coordinate,
                     .height = row.height,
                     .sitePitch = row.getPitch(),
                     .subrows = {}};
        info.subrows.reserve(row.subrows.size());
        for (const ktplace::SubrowInfo &subrow : row.subrows) {
            info.subrows.push_back(
                Subrow{.xlo = subrow.getXlo(), .xhi = subrow.getXhi(row.getPitch())});
        }
        out.push_back(std::move(info));
    }
    return out;
}

Fences copyFences(const ktplace::constraintMgr &constraints) {
    Fences out;
    for (int id = 0; id < static_cast<int>(constraints.getNumRegions()); ++id) {
        const ktplace::Region *source = constraints.getRegion(id);
        if (source == nullptr) {
            continue;
        }
        out.addRegion(
            Region{.name = source->name, .rects = source->rects, .bounds = source->bounds});
    }
    return out;
}

}  // namespace

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
    const BBox *best = nullptr;
    double bestDist = std::numeric_limits<double>::max();
    for (const BBox &cand : r.rects) {
        const double cx = std::clamp(x, cand.lowerLeft.x, cand.upperRight.x);
        const double cy = std::clamp(y, cand.lowerLeft.y, cand.upperRight.y);
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
    x = std::clamp(x, best->lowerLeft.x, best->upperRight.x);
    y = std::clamp(y, best->lowerLeft.y, best->upperRight.y);
    return true;
}

namespace {

bool isInsideAnyRegion(const std::vector<Region> &regions, double x, double y) {
    for (const Region &r : regions) {
        if (r.contains(x, y)) {
            return true;
        }
    }
    return false;
}

double getEscapeEpsilon(const std::vector<Region> &regions) {
    double scale = 0.0;
    for (const Region &r : regions) {
        scale = std::max(scale, std::max(r.bounds.getWidth(), r.bounds.getHeight()));
    }
    return std::max(scale, 1.0) * 1e-6;
}

}  // namespace

bool Fences::pushOutOfRegions(double &x, double &y, const BBox &die) const {
    // Escaping is planned against the *union* of the fences, not one rectangle
    // at a time. Fences are often rings or combs of abutting rectangles, so
    // stepping to the nearest edge of the rectangle that caught the point can
    // drop it into the neighbour and ping-pong forever. Each pass therefore picks
    // the nearest of the four exits that actually lands outside every region,
    // which makes progress monotone.
    const std::size_t maxPasses = 4 * (regions.size() + 1) + 8;
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        const BBox *host = nullptr;
        double hostArea = std::numeric_limits<double>::max();
        for (const Region &r : regions) {
            for (const BBox &cand : r.rects) {
                const double area = cand.getWidth() * cand.getHeight();
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
        const double eps = getEscapeEpsilon(regions);
        const double exits[4][2] = {{host->lowerLeft.x - eps, y},
                                    {host->upperRight.x + eps, y},
                                    {x, host->lowerLeft.y - eps},
                                    {x, host->upperRight.y + eps}};
        const double dists[4] = {x - host->lowerLeft.x, host->upperRight.x - x,
                                 y - host->lowerLeft.y, host->upperRight.y - y};

        int best = -1;
        double bestDist = std::numeric_limits<double>::max();
        bool bestLegal = false;
        for (int k = 0; k < 4; ++k) {
            const bool onDie = die.contains(exits[k][0], exits[k][1]);
            const bool legal = onDie && !isInsideAnyRegion(regions, exits[k][0], exits[k][1]);
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

simplDM::simplDM(Design design, SimplOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

void simplDM::run() {
    simplPlacer placer(d);
    placer.place(opts);
}

simplSolution simplDM::getSolution() const {
    simplSolution out;
    out.xs = d.netlist.fixedX;
    out.ys = d.netlist.fixedY;
    for (std::size_t i = 0; i < d.netlist.numMovable; ++i) {
        out.xs[d.netlist.movVertex[i]] = d.x[i];
        out.ys[d.netlist.movVertex[i]] = d.y[i];
    }
    return out;
}

simplDM buildSimplDM(const ktDM &db) {
    const ktplace::Graph &graph = db.getGraph();
    Design design;
    collectCells(graph, design);
    collectNets(graph, design.netlist);
    design.rows = copyRows(db.getRows());
    design.fences = copyFences(db.getConstraints());
    design.die = getPlacementDieBox(db.getDie(), db.getGraph());

    const StageCounts counts{.cells = graph.getNumCells(),
                             .movable = design.netlist.numMovable,
                             .nets = graph.getNumNets(),
                             .pins = graph.getNumPins(),
                             .rows = design.rows.size()};
    simplDM dm(std::move(design), loadOptions<SimplOptions>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::simpl
