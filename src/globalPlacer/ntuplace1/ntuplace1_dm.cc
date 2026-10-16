#include "globalPlacer/ntuplace1/ntuplace1_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "globalPlacer/ntuplace1/ntuplace1_placer.h"
#include "util/kt_reportTable.h"

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

void reportRun(const Ntuplace1Result &r) {
    ktReportTable t("NTUplace1 solver results");
    t.setHeaders({"metric", "value"});
    t.addRow({"movable cells", std::format("{}", r.numMovable)});
    t.addRow({"fixed blocks", std::format("{}", r.numFixed)});
    t.addRow({"hypergraph nets", std::format("{}", r.nets)});
    t.addRow({"cuts accepted", std::format("{}", r.cuts)});
    t.addRow({"look-ahead retries", std::format("{}", r.ratioRetries)});
    t.addRow({"retries per cut", std::format("{:.3}", r.meanImbalance)});
    t.addRow({"recursion depth reached", std::format("{}", r.maxDepth)});
    t.addRow({"smallest leaf", std::format("{}", r.minLeafCells)});
    t.addRow({"HPWL estimate (pre-legalization)", std::format("{:.6}", r.hpwlFinal)});
    if (r.fenceRegions > 0) {
        t.addRow({"fence regions", std::format("{}", r.fenceRegions)});
        t.addRow({"cells outside their fence", std::format("{}", r.fenceViolations)});
    }
    t.emit();
}

}  // namespace

ntuplace1DM::ntuplace1DM(Design design, Ntuplace1Options options)
    : d(std::move(design)), opts(std::move(options)) {}

Ntuplace1Result ntuplace1DM::run() {
    RatioPlacer placer(d);
    const Ntuplace1Result result = placer.place(opts);
    reportRun(result);
    return result;
}

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
    ntuplace1DM dm(std::move(design), loadOptions<Ntuplace1Options>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::ntuplace1
