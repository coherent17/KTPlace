#include "adaptor/ktToAbacusAdaptor.h"

#include "datamodel/kt_die.h"
#include "visualization/kt_animator.h"
#include "visualization/kt_plotter.h"

#include <algorithm>

namespace ktplace {
namespace {

void copyCells(const Graph &graph, std::vector<abacus::Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const Vertex &cell = graph.getCell(v);
        abacus::Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed;
        c.isTerminal = cell.isTerminal;
    }
}

/// Pins and nets, in the order the design has them, so pin ids line up with the
/// cell ids Abacus already has.
void copyNets(const Graph &graph, std::vector<abacus::Pin> &pins, std::vector<abacus::Net> &nets) {
    pins.resize(graph.getNumPins());
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const Pin &pin = graph.getPin(p);
        abacus::Pin &out = pins[p];
        out.cellId = pin.cellId;
        out.offsetX = pin.offsetX;
        out.offsetY = pin.offsetY;
    }
    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        nets[n].pins = graph.getNetPins(n);
    }
}

void copyRows(const std::vector<RowInfo> &rows, std::vector<abacus::RowInfo> &out) {
    out.reserve(rows.size());
    for (const RowInfo &r : rows) {
        abacus::RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(r.subrows.size());
        for (const SubrowInfo &s : r.subrows) {
            info.subrows.push_back(abacus::Subrow{s.xlo(), s.xhi(r.pitch())});
        }
        out.push_back(std::move(info));
    }
}

void copyFences(const constraintMgr &mgr, abacus::Fences &out) {
    for (int id = 0; id < static_cast<int>(mgr.numRegions()); ++id) {
        const Region *r = mgr.region(id);
        if (r == nullptr) {
            continue;
        }
        abacus::Region region;
        region.rects.reserve(r->rects.size());
        for (const Rect &rect : r->rects) {
            region.rects.push_back(abacus::Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
}

}  // namespace

abacus::abacusDM buildAbacusDM(const ktDM &db, const std::string &plotDir) {
    abacus::Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    // Abacus decides when a frame matters; the drawing stays here, where the
    // shared netlist and the fences live.
    if (!plotDir.empty()) {
        const Graph *graph = &db.getGraph();
        const auto box = design.die;
        const std::array<double, 4> dieBox{box[0], box[1], box[2], box[3]};
        const constraintMgr *fences = &db.constraints();
        const bool animation = PlacementAnimator::instance().enabled();
        design.onFrame = [graph, dieBox, fences, animation](const std::vector<float> &xs,
                                                            const std::vector<float> &ys,
                                                            const abacus::FrameInfo &info) {
            if (!info.path.empty()) {
                writeFrameSvg(info.path, *graph, xs, ys, dieBox, info.step, info.numSteps,
                              info.hpwl, info.hpwlInitial, info.overflow, info.note, fences,
                              /*fixedView=*/true);
            }
            // The same frame into the run's animation, so the GIF shows the
            // legalizer pulling the placement back onto its rows rather than
            // cutting straight from a scattered placement to a legal one.
            if (animation) {
                PlacementAnimator::instance().record(
                    *graph, xs, ys, dieBox, info.step, info.numSteps, info.hpwl, info.hpwlInitial,
                    info.overflow, info.note, fences, info.mandatory);
            }
        };
    }
    return abacus::abacusDM(std::move(design));
}

}  // namespace ktplace
