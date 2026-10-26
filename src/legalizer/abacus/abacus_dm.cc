#include "legalizer/abacus/abacus_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "legalizer/abacus/abacus_legalizer.h"


namespace ktplace::abacus {

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
    return out;
}

std::size_t collectCells(const ktplace::Graph &graph, std::size_t numRegions, Design &design) {
    std::size_t movable = 0;
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        design.vertexX.push_back(cell.x);
        design.vertexY.push_back(cell.y);
        if (cell.isFixed) {
            design.fixedBoxes.push_back(BBox::of(cell.x, cell.y, cell.width, cell.height));
        }
        if (cell.isFixed || cell.isTerminal) {
            continue;
        }
        ++movable;
        Cells &cells = design.cells;
        cells.id.push_back(v);
        cells.x.push_back(cell.x);
        cells.y.push_back(cell.y);
        cells.width.push_back(cell.width);
        cells.height.push_back(cell.height);
        cells.region.push_back(cell.regionId >= 0 &&
                                       static_cast<std::size_t>(cell.regionId) < numRegions
                                   ? cell.regionId
                                   : noRegion);
    }
    return movable;
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
                                  .offX = pin.offsetX,
                                  .offY = pin.offsetY,
                                  .absX = cell.x + pin.offsetX,
                                  .absY = cell.y + pin.offsetY});
        }
        design.nets.add(pins);
    }
}

}  // namespace

abacusDM::abacusDM(Design design, AbacusOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

void abacusDM::run() {
    AbacusLegalizer legalizer(d);
    legalizer.legalize(opts);
}

abacusSolution abacusDM::getSolution() const {
    abacusSolution out;
    out.xs = d.vertexX;
    out.ys = d.vertexY;
    for (std::size_t i = 0; i < d.cells.size(); ++i) {
        out.xs[d.cells.id[i]] = d.cells.x[i];
        out.ys[d.cells.id[i]] = d.cells.y[i];
    }
    return out;
}

abacusDM buildAbacusDM(const ktDM &db) {
    const ktplace::Graph &graph = db.getGraph();
    Design design;
    const std::size_t movable = collectCells(graph, db.getConstraints().getNumRegions(), design);
    collectNets(graph, design);
    design.rows = copyRows(db.getRows(), db.getFencedSubrows());
    design.die = getPlacementDieBox(db.getDie(), graph);

    const StageCounts counts{.cells = graph.getNumCells(),
                             .movable = movable,
                             .nets = graph.getNumNets(),
                             .pins = graph.getNumPins(),
                             .rows = design.rows.size()};
    abacusDM dm(std::move(design), loadOptions<AbacusOptions>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::abacus
