#include "legalizer/abacus/abacus_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "legalizer/abacus/abacus_legalizer.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <format>

namespace ktplace::abacus {

namespace {

std::vector<RowInfo> copyRows(const std::vector<ktplace::RowInfo> &rows,
                              const std::vector<std::vector<ktplace::RowPiece>> &pieces) {
    std::vector<RowInfo> out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ktplace::RowInfo &r = rows[i];
        RowInfo info{r.coordinate, r.height, r.pitch(), {}};
        info.subrows.reserve(pieces[i].size());
        for (const ktplace::RowPiece &p : pieces[i]) {
            info.subrows.push_back(Subrow{p.xlo, p.xhi, p.region});
        }
        out.push_back(std::move(info));
    }
    return out;
}

std::size_t collectCells(const ktplace::Graph &graph, std::size_t numRegions, Design &design) {
    std::size_t movable = 0;
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        design.vertexX.push_back(cell.x);
        design.vertexY.push_back(cell.y);
        if (cell.isFixed) {
            design.fixedBoxes.push_back(
                FixedBox{cell.y, cell.y + cell.height, cell.x, cell.x + cell.width});
        }
        if (cell.isFixed || cell.isTerminal) {
            continue;
        }
        ++movable;
        Cells &cells = design.cells;
        cells.id.push_back(v);
        cells.x.push_back(cell.x);
        cells.y.push_back(cell.y);
        cells.width.push_back(cell.width);
        cells.height.push_back(cell.height);
        cells.region.push_back(cell.regionId >= 0 &&
                                       static_cast<std::size_t>(cell.regionId) < numRegions
                                   ? cell.regionId
                                   : noRegion);
    }
    return movable;
}

void collectNets(const ktplace::Graph &graph, Design &design) {
    std::vector<std::size_t> slotOf(graph.getNumCells(), noSlot);
    for (std::size_t i = 0; i < design.cells.size(); ++i) {
        slotOf[design.cells.id[i]] = i;
    }
    std::vector<NetPin> pins;
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        pins.clear();
        for (const std::size_t pinId : graph.getNetPins(n)) {
            const ktplace::Pin &pin = graph.getPin(pinId);
            const ktplace::Vertex &cell = graph.getCell(pin.cellId);
            pins.push_back(NetPin{slotOf[pin.cellId], pin.offsetX, pin.offsetY,
                                  cell.x + pin.offsetX, cell.y + pin.offsetY});
        }
        design.nets.add(pins);
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
    out.xs = d.vertexX;
    out.ys = d.vertexY;
    for (std::size_t i = 0; i < d.cells.size(); ++i) {
        out.xs[d.cells.id[i]] = d.cells.x[i];
        out.ys[d.cells.id[i]] = d.cells.y[i];
    }
    return out;
}

abacusDM buildAbacusDM(const ktDM &db) {
    const ktplace::Graph &graph = db.getGraph();
    Design design;
    const std::size_t movable = collectCells(graph, db.constraints().numRegions(), design);
    collectNets(graph, design);
    design.rows = copyRows(db.getRows(), db.fencedSubrows());
    design.die = placementDieBox(db.die(), graph);

    const StageCounts counts{graph.getNumCells(), movable, graph.getNumNets(), graph.getNumPins(),
                             design.rows.size()};
    abacusDM dm(std::move(design), loadOptions<AbacusOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::abacus
