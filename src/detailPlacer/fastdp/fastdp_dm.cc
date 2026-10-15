#include "detailPlacer/fastdp/fastdp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_visualizer.h"

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

}  // namespace

fastdpDM buildFastdpDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), db.macroHeight(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), design.rows);
    design.die = placementDieBox(db.die(), db.getGraph());
    const constraintMgr &mgr = db.constraints();
    for (int id = 0; id < static_cast<int>(mgr.numRegions()); ++id) {
        if (const Region *r = mgr.region(id)) {
            for (const Rect &rc : r->rects) {
                design.fences.push_back(FenceRect{rc.lo.x, rc.lo.y, rc.hi.x, rc.hi.y, id});
            }
        }
    }

    // The stage decides when a snapshot matters; the visualizer decides where it
    // lands and what it is called, because it is the one that holds the shared
    // netlist and the output settings.
    // The stage decides when a snapshot matters; the visualizer decides where it
    // lands and what it is called, because it is the one that holds the output
    // settings. It is a singleton, and the netlist and fences belong to the
    // caller, so everything the hook needs outlives the design it is stored in.
    Visualizer &visualizer = Visualizer::instance();
    visualizer.beginStage({"detailplace", "", 0});
    const Graph *netlist = &db.getGraph();
    const constraintMgr *fences = &db.constraints();
    const std::array<double, 4> dieBox = design.die;
    design.onFrame = [&visualizer, netlist, fences, dieBox](const std::vector<float> &xs,
                                                            const std::vector<float> &ys,
                                                            const fastdp::FrameInfo &info) {
        visualizer.frame(*netlist, xs, ys, dieBox, fences,
                         VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                                     info.hpwlInitial, info.overflow, info.mandatory});
    };
    fastdpDM result(std::move(design));
    result.par.localReorderWindow = static_cast<std::size_t>(result.options().window);
    return result;
}

namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside FastDP can act on these, so they are
/// printed here and not returned.
void reportRun(const DetailPlaceResult &res) {
    ktReportTable table("FastDP detailed placement results");
    table.setHeaders({"metric", "value"});
    table.addRow({"global swaps", fmt::format("{}", res.globalSwaps)});
    table.addRow({"vertical swaps", fmt::format("{}", res.verticalSwaps)});
    table.addRow({"median moves", fmt::format("{}", res.medianMoves)});
    table.addRow({"reorder moves", fmt::format("{}", res.reorderMoves)});
    table.addRow({"cluster moves", fmt::format("{}", res.clusterMoves)});
    table.addRow({"HPWL before", fmt::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", fmt::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"overlapping pairs", fmt::format("{}", res.overlappingPairs)});
    table.addRow({"off row", fmt::format("{}", res.offRow)});
    table.addRow({"off site", fmt::format("{}", res.offSite)});
    table.addRow({"over fixed", fmt::format("{}", res.overFixed)});
    table.addRow({"seconds", fmt::format("{:.3f}", res.seconds)});
    table.emit();
}

}  // namespace

fastdpDM::fastdpDM(Design design) : d(std::move(design)) {}

FastdpOptions fastdpDM::options() const {
    return FastdpOptions::load();
}

fastdpSolution fastdpDM::run(const DetailPlaceParams &params) {
    options().report();
    ScopedTimer timer("detailplace");

    FastDetailedPlacer placer(d);
    const DetailPlaceResult result = placer.place(params);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}


}  // namespace ktplace::fastdp
