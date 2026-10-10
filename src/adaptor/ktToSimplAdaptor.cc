#include "adaptor/ktToSimplAdaptor.h"

#include "datamodel/kt_die.h"
#include "visualization/kt_animator.h"
#include "visualization/kt_plotter.h"

#include <algorithm>

namespace ktplace {
namespace {

/// Copy the shared netlist into SimPL's own arrays. This is the whole
/// translation: SimPL needs cells, nets and pins, and nothing else the
/// database carries.
void copyNetlist(const Graph &graph, simpl::Graph &out) {
    const std::size_t numCells = graph.getNumCells();
    const std::size_t numNets = graph.getNumNets();
    const std::size_t numPins = graph.getNumPins();

    for (std::size_t n = 0; n < numNets; ++n) {
        out.addNet(graph.getNet(n).weight);
    }
    for (std::size_t p = 0; p < numPins; ++p) {
        const Pin &pin = graph.getPin(p);
        simpl::Pin outPin;
        outPin.cellId = pin.cellId;
        outPin.netId = pin.netId;
        outPin.role =
            pin.role == PinRole::Driver ? simpl::PinRole::Driver : simpl::PinRole::Receiver;
        outPin.offsetX = pin.offsetX;
        outPin.offsetY = pin.offsetY;
        out.addPin(outPin, pin.netId);
    }
    for (std::size_t v = 0; v < numCells; ++v) {
        const Vertex &cell = graph.getCell(v);
        simpl::Cell outCell;
        outCell.x = cell.x;
        outCell.y = cell.y;
        outCell.width = cell.width;
        outCell.height = cell.height;
        outCell.isFixed = cell.isFixed;
        outCell.isTerminal = cell.isTerminal;
        outCell.regionId = cell.regionId;
        out.addCell(outCell);
    }
}

void copyRows(const std::vector<RowInfo> &rows, std::vector<simpl::RowInfo> &out) {
    out.reserve(rows.size());
    for (const RowInfo &r : rows) {
        simpl::RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(r.subrows.size());
        for (const SubrowInfo &s : r.subrows) {
            info.subrows.push_back(simpl::Subrow{s.xlo(), s.xhi(r.pitch())});
        }
        out.push_back(std::move(info));
    }
}

void copyFences(const constraintMgr &mgr, simpl::Fences &out) {
    for (int id = 0; id < static_cast<int>(mgr.numRegions()); ++id) {
        const Region *r = mgr.region(id);
        if (r == nullptr) {
            continue;
        }
        simpl::Region region;
        region.name = r->name;
        region.rects.reserve(r->rects.size());
        for (const Rect &rect : r->rects) {
            region.rects.push_back(simpl::Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
}

}  // namespace

simpl::simplDM buildSimplDM(const ktDM &db, const std::string &plotDir) {
    simpl::Design design;
    copyNetlist(db.getGraph(), design.graph);
    copyRows(db.getRows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    // SimPL decides when a frame matters; the drawing stays here, where the
    // shared netlist and the fences live.
    if (!plotDir.empty()) {
        const Graph *graph = &db.getGraph();
        const auto box = design.die;
        const std::array<double, 4> dieBox{box[0], box[1], box[2], box[3]};
        const constraintMgr *fences = &db.constraints();
        const bool animation = PlacementAnimator::instance().enabled();
        design.onFrame = [graph, dieBox, fences, animation](const std::vector<float> &xs,
                                                            const std::vector<float> &ys,
                                                            const simpl::FrameInfo &info) {
            if (!info.path.empty()) {
                writeFrameSvg(info.path, *graph, xs, ys, dieBox, info.step, info.numSteps,
                              info.hpwl, info.hpwlInitial, info.overflow, info.note, fences,
                              /*fixedView=*/true);
            }
            // Raster twin for the whole-run animation, so the animation follows
            // the run rather than the SVG names.
            if (animation) {
                PlacementAnimator::instance().record(
                    *graph, xs, ys, dieBox, info.step, info.numSteps, info.hpwl, info.hpwlInitial,
                    info.overflow, info.note, fences, info.mandatory);
            }
        };
    }
    return simpl::simplDM(std::move(design));
}

}  // namespace ktplace
