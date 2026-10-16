#include "detailPlacer/fastdp/fastdp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "detailPlacer/fastdp/fastdp_placer.h"

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

    const StageCounts counts = countStage(design);
    fastdpDM result(std::move(design));
    result.opts = loadOptions<FastdpOptions>();
    reportStageBuild(counts, db.checksum(), result.opts);
    return result;
}


fastdpDM::fastdpDM(Design design) : d(std::move(design)) {}

fastdpSolution fastdpDM::run() {
    FastDetailedPlacer placer(d);
    (void)placer.place(opts);
    return placer.solution();
}


}  // namespace ktplace::fastdp
