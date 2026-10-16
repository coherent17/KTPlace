#include "detailPlacer/fastdp/fastdp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

namespace ktplace::fastdp {


namespace {

void copyCells(const ktplace::Graph &graph, double macroHeight, std::vector<Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed || cell.height > macroHeight;
        c.isTerminal = cell.isTerminal;
        c.region = cell.regionId;
    }
}

/// Pins and nets, in the order the design has them, so pin ids line up with the
/// cell ids MultiRow already has.
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

}  // namespace

fastdpDM buildFastdpDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), db.macroHeight(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), db.fencedSubrows(), design.rows);
    design.die = placementDieBox(db.die(), db.getGraph());

    fastdpDM result(std::move(design));
    result.opts = loadOptions<FastdpOptions>();
    return result;
}

namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside FastDP can act on these, so they are
/// printed here and not returned.
void reportRun(const DetailPlaceResult &res) {
    ktReportTable table("FastDP detailed placement results");
    table.setHeaders({"metric", "value"});
    table.addRow({"global swaps", std::format("{}", res.globalSwaps)});
    table.addRow({"vertical swaps", std::format("{}", res.verticalSwaps)});
    table.addRow({"reorder moves", std::format("{}", res.reorderMoves)});
    table.addRow({"cluster moves", std::format("{}", res.clusterMoves)});
    table.addRow({"HPWL before", std::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", std::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"overlapping pairs", std::format("{}", res.overlappingPairs)});
    table.addRow({"off row", std::format("{}", res.offRow)});
    table.addRow({"off site", std::format("{}", res.offSite)});
    table.addRow({"over fixed", std::format("{}", res.overFixed)});
    table.addRow({"seconds", std::format("{:.3f}", res.seconds)});
    table.emit();
}

}  // namespace

fastdpDM::fastdpDM(Design design) : d(std::move(design)) {}

fastdpSolution fastdpDM::run() {
    reportOptions(opts);
    ScopedTimer timer("detailplace");

    FastDetailedPlacer placer(d);
    const DetailPlaceResult result = placer.place(opts);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}


}  // namespace ktplace::fastdp
