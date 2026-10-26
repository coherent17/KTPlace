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
        RowInfo info{r.coordinate, r.height, r.getPitch(), {}};
        info.subrows.reserve(pieces[i].size());
        for (const ktplace::RowPiece &p : pieces[i]) {
            info.subrows.push_back(Subrow{p.xlo, p.xhi, p.region});
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
        design.cells.push_back(Cell{cell.x, cell.y, cell.width, cell.height, region, isMovable});
        if (isMovable) {
            ++movable;
        } else if (cell.width > 0.0 && cell.height > 0.0) {
            design.blockages.push_back(
                Box{cell.x, cell.y, cell.x + cell.width, cell.y + cell.height});
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
            pins.push_back(NetPin{pin.cellId, pin.offsetX, pin.offsetY});
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
    out.xs.reserve(d.cells.size());
    out.ys.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
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

    const StageCounts counts{graph.getNumCells(), movable, graph.getNumNets(), graph.getNumPins(),
                             design.rows.size()};
    multirowDM dm(std::move(design), loadOptions<MultiRowOptions>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::multirow
