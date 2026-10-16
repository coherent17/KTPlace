#include "placer/simpl/simpl_dm.h"

#include "datamodel/kt_dm.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"


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

simplDM buildSimplDM(const ktDM &db) {
    Design design;
    copyNetlist(db.getGraph(), design.graph);
    copyRows(db.getRows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    simplDM out(std::move(design));
    out.opts = loadOptions<SimplOptions>();
    return out;
}

namespace {

/// The solver's own summary, emitted from the run that produced the numbers
/// rather than by the caller. Nothing outside SimPL can act on these, so they are
/// printed here and not returned.
void reportRun(const SimplResult &res) {
    ktReportTable table("SimPL solver results");
    table.setHeaders({"metric", "initial", "final"});
    table.addRow(
        {"HPWL", std::format("{:.6e}", res.hpwlSeed), std::format("{:.6e}", res.hpwlFinal)});
    table.addRow({"lower bound", "", std::format("{:.6e}", res.hpwlLower)});
    table.addRow({"gap", "", std::format("{:.6e}", res.gap)});
    // Named as it is, not "overflow": the overflow is the bin-density
    // violation scaled by the density limit, which is not the same number as
    // raw overlap, and the qualifier is what says so. CI gates on the label.
    table.addRow({"scaled overflow (lower)", "", std::format("{:.6e}", res.overflowLower)});
    table.addRow({"scaled overflow (final)", "", std::format("{:.6e}", res.overflowFinal)});
    table.addRow({"movable cells", "", std::format("{}", res.numMovable)});
    table.addRow({"fixed cells", "", std::format("{}", res.numFixed)});
    table.addRow({"nets", "", std::format("{}", res.nets)});
    table.addRow({"init iterations", "", std::format("{}", res.initIters)});
    table.addRow({"global iterations", "", std::format("{}", res.globalIters)});
    table.addRow({"fence regions", "", std::format("{}", res.fenceRegions)});
    table.addRow({"fence clamps", "", std::format("{}", res.fenceClamps)});
    table.addRow({"fence pushes", "", std::format("{}", res.fencePushes)});
    table.addRow({"fence violations", "", std::format("{}", res.fenceViolations)});
    table.addRow({"spread seconds", "", std::format("{:.3f}", res.spreadSeconds)});
    table.addRow({"build seconds", "", std::format("{:.3f}", res.buildSeconds)});
    table.addRow({"solve seconds", "", std::format("{:.3f}", res.solveSeconds)});
    table.emit();
}

}  // namespace

simplDM::simplDM(Design design) : d(std::move(design)) {}

simplSolution simplDM::run() {
    ScopedTimer timer("simpl");
    reportOptions(opts);

    simplPlacer placer(d);
    const SimplResult result = placer.place(opts);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}

}  // namespace ktplace::simpl
