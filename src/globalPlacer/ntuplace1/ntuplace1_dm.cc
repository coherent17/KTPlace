// @file ntuplace1_dm.cc
// NTUPlace1's own database

#include "globalPlacer/ntuplace1/ntuplace1_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"

namespace ktplace::ntuplace1 {

namespace {

void copyCells(const ktplace::Graph &graph, std::vector<Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        out[v] = Cell{cell.x,       cell.y,          cell.width,   cell.height,
                      cell.isFixed, cell.isTerminal, cell.regionId};
    }
}

void copyNets(const ktplace::Graph &graph, std::vector<Pin> &pins, std::vector<Net> &nets) {
    pins.resize(graph.getNumPins());
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const ktplace::Pin &pin = graph.getPin(p);
        pins[p] = Pin{pin.cellId, pin.offsetX, pin.offsetY};
    }
    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        nets[n].pins = graph.getNetPins(n);
    }
}

void copyRows(const std::vector<ktplace::RowInfo> &rows, std::vector<RowInfo> &out) {
    out.reserve(rows.size());
    for (const ktplace::RowInfo &r : rows) {
        RowInfo info{r.coordinate, r.height, {}};
        info.subrows.reserve(r.subrows.size());
        for (const ktplace::SubrowInfo &s : r.subrows) {
            info.subrows.push_back(Subrow{s.xlo(), s.xhi(r.pitch())});
        }
        out.push_back(std::move(info));
    }
}

}  // namespace

ntuplace1DM::ntuplace1DM(Design design) : d(std::move(design)) {}

ntuplace1Solution ntuplace1DM::solution() const {
    ntuplace1Solution out;
    out.xs.reserve(d.cells.size());
    out.ys.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
    }
    return out;
}

ntuplace1DM buildNtuplace1DM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), design.rows);
    design.die = placementDieBox(db.die(), db.getGraph());
    design.fences = db.constraints();

    const StageCounts counts = countStage(design);
    ntuplace1DM result(std::move(design));
    result.opts = loadOptions<Ntuplace1Options>();
    reportStageBuild(counts, db.checksum(), result.opts);
    return result;
}

}  // namespace ktplace::ntuplace1
