#include "legalizer/multirow/multirow_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "datamodel/kt_stageReport.h"
#include "legalizer/multirow/multirow_legalizer.h"

#include <algorithm>

namespace ktplace::multirow {

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
            info.subrows.push_back(Subrow{.x = p.x, .region = p.region});
        }
        out.push_back(std::move(info));
    }
    std::stable_sort(out.begin(), out.end(), [](const RowInfo &a, const RowInfo &b) {
        return a.coordinate < b.coordinate;
    });
    return out;
}

std::size_t collectCells(const ktplace::Graph &graph, std::size_t numRegions, Design &design) {
    std::size_t movable = 0;
    design.cells.reserve(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        const bool isMovable = !cell.isFixed && !cell.isTerminal;
        const int region =
            cell.regionId >= 0 && static_cast<std::size_t>(cell.regionId) < numRegions
                ? cell.regionId
                : noRegion;
        design.cells.push_back(Cell{.position = cell.position,
                                    .width = cell.width,
                                    .height = cell.height,
                                    .region = region,
                                    .isMovable = isMovable});
        if (isMovable) {
            ++movable;
        } else if (cell.width > 0.0 && cell.height > 0.0) {
            design.blockages.push_back(
                BBox::of(cell.position.x, cell.position.y, cell.width, cell.height));
        }
    }
    return movable;
}

void collectNets(const ktplace::Graph &graph, Design &design) {
    std::vector<NetPin> pins;
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        pins.clear();
        for (const std::size_t pinId : graph.getNetPins(n)) {
            const ktplace::Pin &pin = graph.getPin(pinId);
            pins.push_back(NetPin{.cellId = pin.cellId, .offset = pin.offset});
        }
        design.nets.add(pins);
    }
}


}  // namespace

multirowDM::multirowDM(Design design, MultiRowOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

void multirowDM::run() {
    MultiRowLegalizer legalizer(d);
    legalizer.legalize(opts);
}

multiRowSolution multirowDM::getSolution() const {
    multiRowSolution out;
    out.positions.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.positions.push_back(cell.position);
    }
    return out;
}

multirowDM buildMultiRowDM(const ktDM &db) {
    Design design;
    const ktplace::Graph &graph = db.getGraph();
    const std::size_t movable = collectCells(graph, db.getConstraints().getNumRegions(), design);
    collectNets(graph, design);
    design.rows = copyRows(db.getRows(), db.getFencedSubrows());
    design.die = getPlacementDieBox(db.getDie(), graph);

    const StageCounts counts{.cells = graph.getNumCells(),
                             .movable = movable,
                             .nets = graph.getNumNets(),
                             .pins = graph.getNumPins(),
                             .rows = design.rows.size()};
    multirowDM dm(std::move(design), loadOptions<MultiRowOptions>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::multirow
