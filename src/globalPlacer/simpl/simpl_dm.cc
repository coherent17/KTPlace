#include "globalPlacer/simpl/simpl_dm.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_macroPlan.h"
#include "datamodel/kt_stageReport.h"
#include "globalPlacer/simpl/simpl_placer.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace ktplace::simpl {
namespace {

PinRole roleOf(ktplace::PinRole role) {
    return role == ktplace::PinRole::Driver ? PinRole::Driver : PinRole::Receiver;
}

struct MacroBias {
    std::vector<double> netScale;
    std::vector<char> centred;
};

void copyNetlist(const ktplace::Graph &source, const MacroBias &bias, Graph &target) {
    for (const ktplace::Net &net : source.getNets()) {
        target.addNet(net.weight * bias.netScale[net.id]);
    }
    for (const ktplace::Pin &pin : source.getPins()) {
        const ktplace::Vertex &cell = source.getCell(pin.cellId);
        const bool centred = bias.centred[pin.cellId] != 0;
        target.addPin(
            Pin{pin.cellId, pin.netId, roleOf(pin.role), centred ? 0.5 * cell.width : pin.offsetX,
                centred ? 0.5 * cell.height : pin.offsetY},
            pin.netId);
    }
    for (std::size_t v = 0; v < source.getNumCells(); ++v) {
        const ktplace::Vertex &cell = source.getCell(v);
        target.addCell(Cell{cell.x, cell.y, cell.width, cell.height, cell.isFixed, cell.isTerminal,
                            cell.regionId});
    }
}

std::vector<RowInfo> copyRows(const std::vector<ktplace::RowInfo> &rows) {
    std::vector<RowInfo> out;
    out.reserve(rows.size());
    for (const ktplace::RowInfo &row : rows) {
        RowInfo info{row.coordinate, row.height, row.pitch(), {}};
        info.subrows.reserve(row.subrows.size());
        for (const ktplace::SubrowInfo &subrow : row.subrows) {
            info.subrows.push_back(Subrow{subrow.xlo(), subrow.xhi(row.pitch())});
        }
        out.push_back(std::move(info));
    }
    return out;
}

Fences copyFences(const ktplace::constraintMgr &constraints) {
    Fences out;
    for (int id = 0; id < static_cast<int>(constraints.numRegions()); ++id) {
        const ktplace::Region *source = constraints.region(id);
        if (source == nullptr) {
            continue;
        }
        Region region;
        region.name = source->name;
        region.rects.reserve(source->rects.size());
        for (const ktplace::Rect &rect : source->rects) {
            region.rects.push_back(Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
    return out;
}

std::string scientific(double value) {
    return std::format("{:.6e}", value);
}

void reportRun(const SimplResult &res) {
    ktReportTable table("SimPL solver results");
    table.setHeaders({"metric", "initial", "final"});
    table.addRow({"HPWL", scientific(res.hpwlSeed), scientific(res.hpwlFinal)});
    table.addRow({"lower bound", "", scientific(res.hpwlLower)});
    table.addRow({"gap", "", scientific(res.gap)});
    table.addRow({"scaled overflow (lower)", "", scientific(res.overflowLower)});
    table.addRow({"scaled overflow (final)", "", scientific(res.overflowFinal)});
    table.addRow({"init iterations", "", std::to_string(res.initIters)});
    table.addRow({"global iterations", "", std::to_string(res.globalIters)});
    if (res.fenceRegions > 0) {
        table.addRow({"fence regions", "", std::to_string(res.fenceRegions)});
        table.addRow({"fence clamps", "", std::to_string(res.fenceClamps)});
        table.addRow({"fence pushes", "", std::to_string(res.fencePushes)});
        table.addRow({"fence violations", "", std::to_string(res.fenceViolations)});
    }
    table.emit();
}

}  // namespace

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

simplDM::simplDM(Design design, SimplOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

SimplResult simplDM::run() {
    simplPlacer placer(d);
    const SimplResult result = placer.place(opts);
    reportRun(result);
    return result;
}

simplSolution simplDM::solution() const {
    simplSolution out;
    out.xs.reserve(d.graph.getNumCells());
    out.ys.reserve(d.graph.getNumCells());
    for (std::size_t v = 0; v < d.graph.getNumCells(); ++v) {
        const Cell &cell = d.graph.getCell(v);
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
    }
    return out;
}

namespace {

MacroBias macroBias(const ktDM &db, const SimplOptions &options) {
    MacroBias bias{.netScale = std::vector<double>(db.getNumNets(), 1.0),
                   .centred = std::vector<char>(db.getNumCells(), 0)};
    if (!db.hasMacroPlan()) {
        return bias;
    }
    const ktplace::Graph &graph = db.getGraph();
    for (const PlannedMacro &macro : db.macroPlan().macros) {
        if (!macro.movable) {
            continue;
        }
        bias.centred[macro.cellId] = options.macroPinCenter ? 1 : 0;
        for (const std::size_t pin : graph.getCellPins(macro.cellId)) {
            bias.netScale[graph.getPin(pin).netId] = options.macroNetWeight;
        }
    }
    return bias;
}

}  // namespace

simplDM buildSimplDM(const ktDM &db) {
    SimplOptions options = loadOptions<SimplOptions>();
    Design design;
    copyNetlist(db.getGraph(), macroBias(db, options), design.graph);
    design.rows = copyRows(db.getRows());
    design.fences = copyFences(db.constraints());
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countGraph(design.graph, design.rows.size());
    simplDM dm(std::move(design), std::move(options));
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::simpl
