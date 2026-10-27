#include "globalPlacer/kraftwerk2/kraftwerk2_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_placer.h"
#include "util/kt_log.h"

namespace ktplace::kraftwerk2 {

namespace {

std::size_t collectCells(const ktplace::Graph &graph, Design &design) {
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        design.vertexX.push_back(cell.position.x);
        design.vertexY.push_back(cell.position.y);
        if (cell.isFixed || cell.isTerminal) {
            if (cell.width > 0.0 && cell.height > 0.0) {
                design.fixedBoxes.push_back(
                    BBox::of(cell.position.x, cell.position.y, cell.width, cell.height));
            }
            continue;
        }
        Cells &cells = design.cells;
        cells.id.push_back(v);
        cells.x.push_back(cell.position.x);
        cells.y.push_back(cell.position.y);
        cells.width.push_back(cell.width);
        cells.height.push_back(cell.height);
    }
    return design.cells.size();
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
            const ktplace::Vertex &cell = graph.getCell(pin.cellId);
            pins.push_back(NetPin{.slot = slotOf[pin.cellId],
                                  .offset = pin.offset,
                                  .fixedPosition = Point(cell.position.x + pin.offset.x,
                                                         cell.position.y + pin.offset.y)});
        }
        design.nets.add(pins);
        design.netWeight.push_back(graph.getNet(n).weight);
    }
}

}  // namespace

kraftwerk2DM::kraftwerk2DM(Design design, Kraftwerk2Options options)
    : d(std::move(design)), opts(std::move(options)) {}

void kraftwerk2DM::run() {
    Kraftwerk2Placer placer(d);
    placer.place(opts);
}

kraftwerk2Solution kraftwerk2DM::getSolution() const {
    kraftwerk2Solution out;
    out.positions.reserve(d.vertexX.size());
    for (std::size_t v = 0; v < d.vertexX.size(); ++v) {
        out.positions.push_back(Point(d.vertexX[v], d.vertexY[v]));
    }
    for (std::size_t i = 0; i < d.cells.size(); ++i) {
        out.positions[d.cells.id[i]] = Point(d.cells.x[i], d.cells.y[i]);
    }
    return out;
}

kraftwerk2DM buildKraftwerk2DM(const ktDM &db) {
    const ktplace::Graph &graph = db.getGraph();
    Design design;
    const std::size_t movable = collectCells(graph, design);
    collectNets(graph, design);
    design.die = getPlacementDieBox(db.getDie(), graph);
    if (db.hasFences()) {
        ktlog.warning("Kraftwerk2 does not model fence regions; legalization enforces them");
    }

    const StageCounts counts{.cells = graph.getNumCells(),
                             .movable = movable,
                             .nets = graph.getNumNets(),
                             .pins = graph.getNumPins(),
                             .rows = db.getRows().size()};
    kraftwerk2DM dm(std::move(design), loadOptions<Kraftwerk2Options>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::kraftwerk2
