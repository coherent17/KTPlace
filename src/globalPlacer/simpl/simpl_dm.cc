#include "globalPlacer/simpl/simpl_dm.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "util/kt_reportTable.h"

#include <format>

namespace ktplace::simpl {
namespace {

PinRole roleOf(ktplace::PinRole role) {
    return role == ktplace::PinRole::Driver ? PinRole::Driver : PinRole::Receiver;
}

void copyNetlist(const ktplace::Graph &source, Graph &target) {
    for (const ktplace::Net &net : source.getNets()) {
        target.addNet(net.weight);
    }
    for (const ktplace::Pin &pin : source.getPins()) {
        target.addPin(Pin{pin.cellId, pin.netId, roleOf(pin.role), pin.offsetX, pin.offsetY},
                      pin.netId);
    }
    for (std::size_t v = 0; v < source.getNumCells(); ++v) {
        const ktplace::Vertex &cell = source.getCell(v);
        target.addCell(Cell{cell.x, cell.y, cell.width, cell.height, cell.isFixed, cell.isTerminal,
                            cell.regionId});
    }
}

std::vector<RowInfo> copyRows(const std::vector<ktplace::RowInfo> &rows) {
    std::vector<RowInfo> out;
    out.reserve(rows.size());
    for (const ktplace::RowInfo &row : rows) {
        RowInfo info{row.coordinate, row.height, row.pitch(), {}};
        info.subrows.reserve(row.subrows.size());
        for (const ktplace::SubrowInfo &subrow : row.subrows) {
            info.subrows.push_back(Subrow{subrow.xlo(), subrow.xhi(row.pitch())});
        }
        out.push_back(std::move(info));
    }
    return out;
}

Fences copyFences(const ktplace::constraintMgr &constraints) {
    Fences out;
    for (int id = 0; id < static_cast<int>(constraints.numRegions()); ++id) {
        const ktplace::Region *source = constraints.region(id);
        if (source == nullptr) {
            continue;
        }
        Region region;
        region.name = source->name;
        region.rects.reserve(source->rects.size());
        for (const ktplace::Rect &rect : source->rects) {
            region.rects.push_back(Rect{rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
        region.seal();
        out.addRegion(std::move(region));
    }
    return out;
}

std::string scientific(double value) {
    return std::format("{:.6e}", value);
}

void reportRun(const SimplResult &res) {
    ktReportTable table("SimPL solver results");
    table.setHeaders({"metric", "initial", "final"});
    table.addRow({"HPWL", scientific(res.hpwlSeed), scientific(res.hpwlFinal)});
    table.addRow({"lower bound", "", scientific(res.hpwlLower)});
    table.addRow({"gap", "", scientific(res.gap)});
    table.addRow({"scaled overflow (lower)", "", scientific(res.overflowLower)});
    table.addRow({"scaled overflow (final)", "", scientific(res.overflowFinal)});
    table.addRow({"init iterations", "", std::to_string(res.initIters)});
    table.addRow({"global iterations", "", std::to_string(res.globalIters)});
    if (res.fenceRegions > 0) {
        table.addRow({"fence regions", "", std::to_string(res.fenceRegions)});
        table.addRow({"fence clamps", "", std::to_string(res.fenceClamps)});
        table.addRow({"fence pushes", "", std::to_string(res.fencePushes)});
        table.addRow({"fence violations", "", std::to_string(res.fenceViolations)});
    }
    table.emit();
}

}  // namespace

simplDM::simplDM(Design design, SimplOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

SimplResult simplDM::run() {
    simplPlacer placer(d);
    const SimplResult result = placer.place(opts);
    reportRun(result);
    return result;
}

void simplDM::commit(ktDM &db) const {
    for (std::size_t v = 0; v < d.graph.getNumCells(); ++v) {
        const Cell &cell = d.graph.getCell(v);
        db.setCellPosition(v, cell.x, cell.y);
    }
}

simplDM buildSimplDM(const ktDM &db) {
    Design design;
    copyNetlist(db.getGraph(), design.graph);
    design.rows = copyRows(db.getRows());
    design.fences = copyFences(db.constraints());
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countGraph(design.graph, design.rows.size());
    simplDM dm(std::move(design), loadOptions<SimplOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::simpl
