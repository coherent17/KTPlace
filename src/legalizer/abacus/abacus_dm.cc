#include "legalizer/abacus/abacus_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "legalizer/abacus/abacus_legalizer.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <format>
#include <limits>

namespace ktplace::abacus {

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
        c.region = cell.regionId;
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

void reportRun(const AbacusResult &r) {
    ktReportTable t("Legalization (Abacus)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells placed", std::format("{}", r.cellsPlaced)});
    t.addRow({"cells unplaced", std::format("{}", r.unplaced)});
    t.addRow({"squared displacement", std::format("{:.6e}", r.totalSquaredDisplacement)});
    t.addRow({"max displacement", std::format("{:.1f}", r.maxDisplacement)});
    t.addRow({"HPWL before", std::format("{:.6e}", r.hpwlBefore)});
    t.addRow({"HPWL after", std::format("{:.6e}", r.hpwlAfter)});
    t.addRow(
        {"HPWL change", std::format("{:+.2f}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                                    (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"seconds", std::format("{:.3f}", r.seconds)});
    t.addRow({"overlapping pairs", std::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", std::format("{}", r.offRow)});
    t.addRow({"cells off site", std::format("{}", r.offSite)});
    t.addRow({"cells over macro", std::format("{}", r.overFixed)});
    t.addRow({"cells out of rows", std::format("{}", r.outOfRows)});
    t.addRow({"commit failures", std::format("{}", r.commitFailures)});
    t.emit();

    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }
    if (r.outOfRows > 0) {
        ktlog.warning(
            "{} cell(s) could not be placed in any row and are still at their global placement "
            "positions (no room within 'abacus max_row_dist' rows, or taller than a row). "
            "The placement is not legal; see \"cells out of rows\" above.",
            r.outOfRows);
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

abacusDM::abacusDM(Design design, AbacusOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

AbacusResult abacusDM::run() {
    AbacusLegalizer legalizer(d);
    const AbacusResult result = legalizer.legalize(opts);
    reportRun(result);
    return result;
}

abacusSolution abacusDM::solution() const {
    abacusSolution out;
    out.xs.reserve(d.cells.size());
    out.ys.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
    }
    return out;
}

abacusDM buildAbacusDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), db.fencedSubrows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countStage(design);
    abacusDM dm(std::move(design), loadOptions<AbacusOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::abacus
