#include "legalizer/multirow/multirow_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "datamodel/kt_stageReport.h"
#include "legalizer/multirow/multirow_legalizer.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

namespace ktplace::multirow {

namespace {

void copyCells(const ktplace::Graph &graph, std::vector<Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed;
        c.isTerminal = cell.isTerminal;
        c.regionId = cell.regionId;
    }
}

void copyNets(const ktplace::Graph &graph, std::vector<Pin> &pins, std::vector<Net> &nets) {
    pins.resize(graph.getNumPins());
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const ktplace::Pin &pin = graph.getPin(p);
        Pin &out = pins[p];
        out.cellId = pin.cellId;
        out.offsetX = pin.offsetX;
        out.offsetY = pin.offsetY;
    }
    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        nets[n].pins = graph.getNetPins(n);
    }
}

void copyRows(const std::vector<ktplace::RowInfo> &rows,
              const std::vector<std::vector<ktplace::RowPiece>> &pieces,
              std::vector<RowInfo> &out) {
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ktplace::RowInfo &r = rows[i];
        RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(pieces[i].size());
        for (const ktplace::RowPiece &p : pieces[i]) {
            info.subrows.push_back(Subrow{p.xlo, p.xhi, p.region});
        }
        out.push_back(std::move(info));
    }
}

void copyFences(const ktplace::constraintMgr &mgr, Fences &out) {
    for (int id = 0; id < static_cast<int>(mgr.numRegions()); ++id) {
        const ktplace::Region *r = mgr.region(id);
        if (r == nullptr) {
            continue;
        }
        Region region;
        region.rects.reserve(r->rects.size());
        for (const ktplace::Rect &rect : r->rects) {
            region.rects.push_back(Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
}

}  // namespace

namespace {

void reportRun(const MultiRowResult &res) {
    ktReportTable table("Legalization (multi-row)");
    table.setHeaders({"metric", "value"});
    table.addRow({"movable cells", std::format("{}", res.movable)});
    table.addRow({"cells placed", std::format("{}", res.placed)});
    table.addRow({"cells unplaced", std::format("{}", res.unplaced)});
    table.addRow({"cells taller than a row", std::format("{}", res.multiRow)});
    table.addRow({"of those, placed", std::format("{}", res.multiRowPlaced)});
    table.addRow({"macros legalized first", std::format("{}", res.macros)});
    table.addRow({"macros placed", std::format("{}", res.macrosPlaced)});
    table.addRow({"total displacement", std::format("{:.6e}", res.displacement)});
    table.addRow({"max displacement", std::format("{:.1f}", res.maxDisplacement)});
    table.addRow({"HPWL before", std::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", std::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"seconds", std::format("{:.3f}", res.seconds)});
    table.emit();
    if (res.unplaced > 0) {
        ktlog.warning("{} cell(s) found no legal position and kept their global placement",
                      res.unplaced);
    }
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

double netlistHPWL(const std::vector<Cell> &, const std::vector<Net> &nets,
                   const std::vector<Pin> &pins, const std::vector<double> &xs,
                   const std::vector<double> &ys) {
    double total = 0.0;
    for (const Net &net : nets) {
        if (net.pins.size() < 2) {
            continue;
        }
        double xlo = std::numeric_limits<double>::max();
        double xhi = std::numeric_limits<double>::lowest();
        double ylo = std::numeric_limits<double>::max();
        double yhi = std::numeric_limits<double>::lowest();
        for (const std::size_t pinId : net.pins) {
            const Pin &pin = pins.at(pinId);
            const double px = xs.at(pin.cellId) + pin.offsetX;
            const double py = ys.at(pin.cellId) + pin.offsetY;
            xlo = std::min(xlo, px);
            xhi = std::max(xhi, px);
            ylo = std::min(ylo, py);
            yhi = std::max(yhi, py);
        }
        total += (xhi - xlo) + (yhi - ylo);
    }
    return total;
}

multirowDM::multirowDM(Design design, MultiRowOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

MultiRowResult multirowDM::run() {
    MultiRowLegalizer legalizer(d);
    const MultiRowResult result = legalizer.legalize(opts);
    reportRun(result);
    return result;
}

multiRowSolution multirowDM::solution() const {
    multiRowSolution out;
    out.xs.reserve(d.cells.size());
    out.ys.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
    }
    return out;
}

multirowDM buildMultiRowDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), db.fencedSubrows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countStage(design);
    multirowDM dm(std::move(design), loadOptions<MultiRowOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::multirow
