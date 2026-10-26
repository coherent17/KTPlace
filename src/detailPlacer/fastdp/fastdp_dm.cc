#include "detailPlacer/fastdp/fastdp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "detailPlacer/fastdp/fastdp_placer.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ktplace::fastdp {

namespace {

std::vector<RowInfo> copyRows(const std::vector<ktplace::RowInfo> &rows,
                              const std::vector<std::vector<ktplace::RowPiece>> &pieces) {
    std::vector<RowInfo> out;
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ktplace::RowInfo &r = rows[i];
        RowInfo info{.coordinate = r.coordinate,
                     .height = r.height,
                     .sitePitch = r.getPitch(),
                     .subrows = {}};
        info.subrows.reserve(pieces[i].size());
        for (const ktplace::RowPiece &p : pieces[i]) {
            info.subrows.push_back(Subrow{.xlo = p.xlo, .xhi = p.xhi, .region = p.region});
        }
        out.push_back(std::move(info));
    }
    std::ranges::stable_sort(out, {}, &RowInfo::coordinate);
    return out;
}

double getRowHeightOf(const std::vector<RowInfo> &rows) {
    double height = std::numeric_limits<double>::infinity();
    for (const RowInfo &row : rows) {
        if (row.height > 0.0) {
            height = std::min(height, row.height);
        }
    }
    return std::isfinite(height) ? height : 1.0;
}

std::size_t collectCells(const ktplace::Graph &graph, double macroHeight, Design &design) {
    const double rowHeight = getRowHeightOf(design.rows);
    std::size_t placeable = 0;
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        const bool fixed = cell.isFixed || cell.height > macroHeight;
        const bool tall = cell.height > rowHeight + eps;
        design.vertexX.push_back(cell.position.x);
        design.vertexY.push_back(cell.position.y);
        placeable += fixed || cell.isTerminal ? 0 : 1;
        const BBox box = BBox::of(cell.position.x, cell.position.y, cell.width, cell.height);
        if (fixed || tall) {
            design.fixedBoxes.push_back(box);
        }
        if (fixed || cell.isTerminal || tall) {
            design.tallCells += tall && !fixed && !cell.isTerminal ? 1 : 0;
            if (cell.width > 0.0 && cell.height > 0.0) {
                design.blockages.push_back(box);
            }
            continue;
        }
        Cells &cells = design.cells;
        cells.id.push_back(v);
        cells.x.push_back(cell.position.x);
        cells.y.push_back(cell.position.y);
        cells.width.push_back(cell.width);
        cells.height.push_back(cell.height);
        cells.region.push_back(cell.regionId);
    }
    return placeable;
}

void collectNets(const ktplace::Graph &graph, Design &design) {
    std::vector<std::size_t> slotOf(graph.getNumCells(), noSlot);
    for (std::size_t i = 0; i < design.cells.size(); ++i) {
        slotOf[design.cells.id[i]] = i;
    }
    std::vector<NetPin> pins;
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        pins.clear();
        for (const std::size_t pinId : graph.getNetPins(n)) {
            const ktplace::Pin &pin = graph.getPin(pinId);
            const std::size_t slot = slotOf[pin.cellId];
            if (slot == noSlot) {
                const ktplace::Vertex &cell = graph.getCell(pin.cellId);
                pins.push_back(NetPin{.slot = noSlot,
                                      .offset = pin.offset,
                                      .fixedPosition = {.x = cell.position.x + pin.offset.x,
                                                        .y = cell.position.y + pin.offset.y}});
            } else {
                pins.push_back(NetPin{.slot = slot, .offset = pin.offset});
            }
        }
        design.nets.add(pins);
    }
}

}  // namespace

fastdpDM::fastdpDM(Design design, FastdpOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

void fastdpDM::run() {
    FastDetailedPlacer placer(d);
    placer.place(opts);
}

fastdpSolution fastdpDM::getSolution() const {
    fastdpSolution out;
    out.positions.reserve(d.vertexX.size());
    for (std::size_t v = 0; v < d.vertexX.size(); ++v) {
        out.positions.push_back({.x = d.vertexX[v], .y = d.vertexY[v]});
    }
    for (std::size_t i = 0; i < d.cells.size(); ++i) {
        out.positions[d.cells.id[i]] = {.x = d.cells.x[i], .y = d.cells.y[i]};
    }
    return out;
}

fastdpDM buildFastdpDM(const ktDM &db) {
    const ktplace::Graph &graph = db.getGraph();
    Design design;
    design.rows = copyRows(db.getRows(), db.getFencedSubrows());
    const std::size_t placeable = collectCells(graph, db.getMacroHeight(), design);
    collectNets(graph, design);
    design.die = getPlacementDieBox(db.getDie(), graph);

    const StageCounts counts{.cells = graph.getNumCells(),
                             .movable = placeable,
                             .nets = graph.getNumNets(),
                             .pins = graph.getNumPins(),
                             .rows = design.rows.size()};
    fastdpDM dm(std::move(design), loadOptions<FastdpOptions>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::fastdp
