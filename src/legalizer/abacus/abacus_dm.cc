#include "legalizer/abacus/abacus_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_visualizer.h"

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

/// Pins and nets, in the order the design has them, so pin ids line up with the
/// cell ids Abacus already has.
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

void copyRows(const std::vector<ktplace::RowInfo> &rows, std::vector<RowInfo> &out) {
    out.reserve(rows.size());
    for (const ktplace::RowInfo &r : rows) {
        RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(r.subrows.size());
        for (const ktplace::SubrowInfo &s : r.subrows) {
            info.subrows.push_back(Subrow{s.xlo(), s.xhi(r.pitch())});
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

abacusDM buildAbacusDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    // The stage decides when a snapshot matters; the visualizer decides where it
    // lands and what it is called, because it is the one that holds the shared
    // netlist and the output settings.
    // The stage decides when a snapshot matters; the visualizer decides where it
    // lands and what it is called, because it is the one that holds the output
    // settings. It is a singleton, and the netlist and fences belong to the
    // caller, so everything the hook needs outlives the design it is stored in.
    Visualizer &visualizer = Visualizer::instance();
    visualizer.beginStage({"legalize", "", 0});
    const Graph *netlist = &db.getGraph();
    const constraintMgr *fences = &db.constraints();
    const std::array<double, 4> dieBox = design.die;
    design.onFrame = [&visualizer, netlist, fences, dieBox](const std::vector<float> &xs,
                                                            const std::vector<float> &ys,
                                                            const abacus::FrameInfo &info) {
        visualizer.frame(*netlist, xs, ys, dieBox, fences,
                         VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                                     info.hpwlInitial, info.overflow, info.mandatory});
    };
    abacusDM result(std::move(design));
    // Only when something sets it: the legalizer's own default, 0, searches
    // every row, and the option's 16 would cap that on every run.
    if (const auto v = Option<std::size_t>{"abacus", "max_row_dist", 16}.tryGet()) {
        result.par.maxRowDistance = *v;
    }
    return result;
}

namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside Abacus can act on these, so they are
/// printed here and not returned.
void reportRun(const LegalizeResult &res) {
    ktReportTable table("Abacus legalization results");
    table.setHeaders({"metric", "value"});
    table.addRow({"cells placed", fmt::format("{}", res.cellsPlaced)});
    table.addRow({"unplaced", fmt::format("{}", res.unplaced)});
    table.addRow({"HPWL before", fmt::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", fmt::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"total displacement", fmt::format("{:.6e}", res.totalSquaredDisplacement)});
    table.addRow({"max displacement", fmt::format("{:.6}", res.maxDisplacement)});
    table.addRow({"overlapping pairs", fmt::format("{}", res.overlappingPairs)});
    table.addRow({"outside rows", fmt::format("{}", res.outOfRows)});
    table.addRow({"over fixed", fmt::format("{}", res.overFixed)});
    table.addRow({"commit failures", fmt::format("{}", res.commitFailures)});
    table.addRow({"seconds", fmt::format("{:.3f}", res.seconds)});
    table.emit();
}

}  // namespace

abacusDM::abacusDM(Design design) : d(std::move(design)) {}

AbacusOptions abacusDM::options() const {
    return AbacusOptions::load();
}

abacusSolution abacusDM::run(const LegalizeParams &params) {
    options().report();
    ScopedTimer timer("legalize");

    AbacusLegalizer legalizer(d);
    const LegalizeResult result = legalizer.legalize(params);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return legalizer.solution();
}


}  // namespace ktplace::abacus
