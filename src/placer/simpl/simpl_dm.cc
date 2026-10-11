#include "placer/simpl/simpl_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_visualizer.h"

#include <algorithm>

namespace ktplace::simpl {
namespace {

/// Copy the shared netlist into SimPL's own arrays. This is the whole
/// translation: SimPL needs cells, nets and pins, and nothing else the
/// database carries.
void copyNetlist(const ktplace::Graph &graph, Graph &out) {
    const std::size_t numNets = graph.getNumNets();
    const std::size_t numPins = graph.getNumPins();
    for (std::size_t n = 0; n < numNets; ++n) {
        out.addNet(graph.getNet(n).weight);
    }
    for (std::size_t p = 0; p < numPins; ++p) {
        const ktplace::Pin &pin = graph.getPin(p);
        out.addPin(Pin{pin.cellId, pin.netId,
                       pin.role == ktplace::PinRole::Driver ? PinRole::Driver : PinRole::Receiver,
                       pin.offsetX, pin.offsetY},
                   pin.netId);
    }
    const std::size_t numCells = graph.getNumCells();
    for (std::size_t v = 0; v < numCells; ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        out.addCell(Cell{cell.x, cell.y, cell.width, cell.height, cell.isFixed, cell.isTerminal,
                         cell.regionId});
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
        region.name = r->name;
        region.rects.reserve(r->rects.size());
        for (const ktplace::Rect &rect : r->rects) {
            region.rects.push_back(Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
}

}  // namespace

simplDM buildSimplDM(const ktDM &db, const std::string &plotDir) {
    Design design;
    copyNetlist(db.getGraph(), design.graph);
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
    visualizer.beginStage({"simpl", "simpl", 0});
    const ktplace::Graph *netlist = &db.getGraph();
    const ktplace::constraintMgr *fences = &db.constraints();
    const std::array<double, 4> dieBox = design.die;
    design.onFrame = [&visualizer, netlist, fences, dieBox](const std::vector<float> &xs,
                                                            const std::vector<float> &ys,
                                                            const simpl::FrameInfo &info) {
        visualizer.frame(*netlist, xs, ys, dieBox, fences,
                         VisualFrame{info.series, info.step, info.numSteps, info.note, info.hpwl,
                                     info.hpwlInitial, info.overflow, info.mandatory});
    };
    // SimPL also draws diagnostics of its own -- bin-density maps -- which are
    // SVG text rather than pictures of a placement, so they take the
    // visualizer's raw-write path instead of the cell-frame one.
    design.onSvg = [&visualizer](const std::string &series, const std::string &step,
                                 const std::string &body) {
        visualizer.writeSvg(series, step, body);
    };
    design.artifactDir = plotDir;
    return simplDM(std::move(design));
}

namespace {

/// The solver's own summary, emitted from the run that produced the numbers
/// rather than by the caller. Nothing outside SimPL can act on these, so they are
/// printed here and not returned.
void reportRun(const SimplResult &res) {
    ktReportTable table("SimPL solver results");
    table.setHeaders({"metric", "initial", "final"});
    table.addRow(
        {"HPWL", fmt::format("{:.6e}", res.hpwlSeed), fmt::format("{:.6e}", res.hpwlFinal)});
    table.addRow({"lower bound", "", fmt::format("{:.6e}", res.hpwlLower)});
    table.addRow({"gap", "", fmt::format("{:.6e}", res.gap)});
    table.addRow({"overflow", "", fmt::format("{:.6e}", res.overflowFinal)});
    table.addRow({"movable cells", "", fmt::format("{}", res.numMovable)});
    table.addRow({"fixed cells", "", fmt::format("{}", res.numFixed)});
    table.addRow({"nets", "", fmt::format("{}", res.nets)});
    table.addRow({"init iterations", "", fmt::format("{}", res.initIters)});
    table.addRow({"global iterations", "", fmt::format("{}", res.globalIters)});
    table.addRow({"fence regions", "", fmt::format("{}", res.fenceRegions)});
    table.addRow({"fence clamps", "", fmt::format("{}", res.fenceClamps)});
    table.addRow({"fence pushes", "", fmt::format("{}", res.fencePushes)});
    table.addRow({"fence violations", "", fmt::format("{}", res.fenceViolations)});
    table.addRow({"spread seconds", "", fmt::format("{:.3f}", res.spreadSeconds)});
    table.addRow({"build seconds", "", fmt::format("{:.3f}", res.buildSeconds)});
    table.addRow({"solve seconds", "", fmt::format("{:.3f}", res.solveSeconds)});
    table.emit();
}

}  // namespace

simplDM::simplDM(Design design) : d(std::move(design)) {}

simplSolution simplDM::run(const SimplParams &params, bool useFences) {
    ScopedTimer timer("simpl");

    simplPlacer placer(d);
    const SimplResult result = placer.place(params, useFences);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}

}  // namespace ktplace::simpl
