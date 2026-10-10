#include "adaptor/ktToFastdpAdaptor.h"

#include "datamodel/kt_die.h"
#include "visualization/kt_animator.h"
#include "visualization/kt_plotter.h"

#include <algorithm>

namespace ktplace {
namespace {

void copyCells(const Graph &graph, std::vector<fastdp::Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const Vertex &cell = graph.getCell(v);
        fastdp::Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed;
        c.isTerminal = cell.isTerminal;
    }
}

/// Pins and nets, in the order the design has them, so pin ids line up with the
/// cell ids MultiRow already has.
void copyNets(const Graph &graph, std::vector<fastdp::Pin> &pins, std::vector<fastdp::Net> &nets) {
    pins.resize(graph.getNumPins());
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const Pin &pin = graph.getPin(p);
        fastdp::Pin &out = pins[p];
        out.cellId = pin.cellId;
        out.offsetX = pin.offsetX;
        out.offsetY = pin.offsetY;
    }
    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        nets[n].pins = graph.getNetPins(n);
    }
}

void copyRows(const std::vector<RowInfo> &rows, std::vector<fastdp::RowInfo> &out) {
    out.reserve(rows.size());
    for (const RowInfo &r : rows) {
        fastdp::RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(r.subrows.size());
        for (const SubrowInfo &s : r.subrows) {
            info.subrows.push_back(fastdp::Subrow{s.xlo(), s.xhi(r.pitch())});
        }
        out.push_back(std::move(info));
    }
}

}  // namespace

fastdp::fastdpDM buildFastdpDM(const ktDM &db, const std::string &plotDir) {
    fastdp::Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), design.rows);
    design.die = placementDieBox(db.die(), db.getGraph());

    // MultiRow decides when a frame matters; the drawing stays here, where the
    // shared netlist and the fences live.
    if (!plotDir.empty()) {
        const Graph *graph = &db.getGraph();
        const auto box = design.die;
        const std::array<double, 4> dieBox{box[0], box[1], box[2], box[3]};
        const bool animation = PlacementAnimator::instance().enabled();
        design.onFrame = [graph, dieBox, animation](const std::vector<float> &xs,
                                                    const std::vector<float> &ys,
                                                    const fastdp::FrameInfo &info) {
            if (!info.path.empty()) {
                writeFrameSvg(info.path, *graph, xs, ys, dieBox, info.step, info.numSteps,
                              info.hpwl, info.hpwlInitial, info.overflow, info.note, nullptr,
                              /*fixedView=*/true);
            }
            // The same frame into the run's animation, so detailed placement's
            // contribution -- usually the last thing that moves cells -- is in the
            // GIF too.
            if (animation) {
                PlacementAnimator::instance().record(
                    *graph, xs, ys, dieBox, info.step, info.numSteps, info.hpwl, info.hpwlInitial,
                    info.overflow, info.note, nullptr, info.mandatory);
            }
        };
    }
    return fastdp::fastdpDM(std::move(design));
}

}  // namespace ktplace
