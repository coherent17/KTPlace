#include "legalizer/abacus/abacus_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

namespace ktplace::abacus {


namespace {

void copyCells(const ktplace::Graph &graph, std::vector<Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed;
        c.isTerminal = cell.isTerminal;
        c.region = cell.regionId;
    }
}

/// Pins and nets, in the order the design has them, so pin ids line up with the
/// cell ids Abacus already has.
void copyNets(const ktplace::Graph &graph, std::vector<Pin> &pins, std::vector<Net> &nets) {
    pins.resize(graph.getNumPins());
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const ktplace::Pin &pin = graph.getPin(p);
        Pin &out = pins[p];
        out.cellId = pin.cellId;
        out.offsetX = pin.offsetX;
        out.offsetY = pin.offsetY;
    }
    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < graph.getNumNets(); ++n) {
        nets[n].pins = graph.getNetPins(n);
    }
}

void copyRows(const std::vector<ktplace::RowInfo> &rows,
              const std::vector<std::vector<ktplace::RowPiece>> &pieces,
              std::vector<RowInfo> &out) {
    out.reserve(rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const ktplace::RowInfo &r = rows[i];
        RowInfo info;
        info.coordinate = r.coordinate;
        info.height = r.height;
        info.sitePitch = r.pitch();
        info.subrows.reserve(pieces[i].size());
        for (const ktplace::RowPiece &p : pieces[i]) {
            info.subrows.push_back(Subrow{p.xlo, p.xhi, p.region});
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
        region.rects.reserve(r->rects.size());
        for (const ktplace::Rect &rect : r->rects) {
            region.rects.push_back(Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
}

}  // namespace

abacusDM buildAbacusDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), db.fencedSubrows(), design.rows);
    copyFences(db.constraints(), design.fences);
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countStage(design);
    abacusDM result(std::move(design));
    result.opts = loadOptions<AbacusOptions>();
    reportStageBuild(counts, db.checksum(), result.opts);
    return result;
}


abacusDM::abacusDM(Design design) : d(std::move(design)) {}

abacusSolution abacusDM::run() {
    AbacusLegalizer legalizer(d);
    (void)legalizer.legalize(opts);
    return legalizer.solution();
}


}  // namespace ktplace::abacus
