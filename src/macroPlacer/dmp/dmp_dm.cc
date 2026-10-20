#include "macroPlacer/dmp/dmp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_macroPlan.h"
#include "datamodel/kt_optionDb.h"
#include "datamodel/kt_stageReport.h"
#include "macroPlacer/dmp/dmp_placer.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <utility>
#include <vector>

namespace ktplace::dmp {

namespace {

void reportRun(const DmpResult &r) {
    ktReportTable t("Detailed macro placement");
    t.setHeaders({"metric", "value"});
    t.addRow({"passes", std::format("{}", r.passes)});
    t.addRow({"macros moved", std::format("{}", r.moved)});
    t.addRow({"macros flipped", std::format("{}", r.flipped)});
    t.addRow({"macro-net HPWL before", std::format("{:.6e}", r.hpwlBefore)});
    t.addRow({"macro-net HPWL after", std::format("{:.6e}", r.hpwlAfter)});
    t.addRow({"macro-net HPWL change",
              std::format("{:+.2f}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                          (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"seconds", std::format("{:.3f}", r.seconds)});
    t.emit();
}

}  // namespace

dmpDM::dmpDM(Design design, DmpOptions options) : d(std::move(design)), opts(options) {}

DmpResult dmpDM::run() {
    const DmpResult result = placeMacros(d, opts);
    reportRun(result);
    return result;
}

dmpSolution dmpDM::solution() const {
    dmpSolution out;
    for (const Macro &macro : d.macros) {
        if (!macro.movable) {
            continue;
        }
        out.cells.push_back(macro.cellId);
        out.xs.push_back(macro.x);
        out.ys.push_back(macro.y);
        out.orients.push_back(macro.orient);
    }
    return out;
}

dmpDM buildDmpDM(const ktDM &db) {
    const MacroPlan &plan = db.macroPlan();
    const Graph &graph = db.getGraph();
    Design design;
    design.die = plan.die;
    design.rules = plan.rules;
    if (!db.getRows().empty()) {
        const ktplace::RowInfo &row = db.getRows().front();
        design.grid = {.originX = row.xlo(),
                       .pitchX = row.pitch(),
                       .originY = row.coordinate,
                       .pitchY = row.height};
    }

    std::vector<int> macroOfCell(graph.getNumCells(), noMacro);
    for (const PlannedMacro &planned : plan.macros) {
        const Vertex &cell = graph.getCell(planned.cellId);
        macroOfCell[planned.cellId] = static_cast<int>(design.macros.size());
        design.macros.push_back({.cellId = planned.cellId,
                                 .x = cell.x,
                                 .y = cell.y,
                                 .width = cell.width,
                                 .height = cell.height,
                                 .orient = cell.orient,
                                 .movable = planned.movable,
                                 .originX = planned.originX,
                                 .originY = planned.originY,
                                 .originOrient = planned.originOrient});
    }

    std::vector<std::size_t> nets;
    for (const Macro &macro : design.macros) {
        if (!macro.movable) {
            continue;
        }
        for (const std::size_t p : graph.getCellPins(macro.cellId)) {
            nets.push_back(graph.getPin(p).netId);
        }
    }
    std::ranges::sort(nets);
    const auto [first, last] = std::ranges::unique(nets);
    nets.erase(first, last);

    for (const std::size_t n : nets) {
        const std::vector<std::size_t> &pinIds = graph.getNetPins(n);
        if (pinIds.size() < 2) {
            continue;
        }
        Net net;
        for (const std::size_t p : pinIds) {
            const ktplace::Pin &pin = graph.getPin(p);
            const int m = macroOfCell[pin.cellId];
            const bool moves = m != noMacro && design.macros[static_cast<std::size_t>(m)].movable;
            const Vertex &cell = graph.getCell(pin.cellId);
            net.pins.push_back(design.pins.size());
            design.pins.push_back(
                moves
                    ? Pin{.macro = m, .x = pin.offsetX, .y = pin.offsetY}
                    : Pin{.macro = noMacro, .x = cell.x + pin.offsetX, .y = cell.y + pin.offsetY});
        }
        design.nets.push_back(std::move(net));
    }

    const std::size_t movable = plan.movableCount();
    const StageCounts counts{.cells = design.macros.size(),
                             .movable = movable,
                             .nets = design.nets.size(),
                             .pins = design.pins.size(),
                             .rows = 0};
    dmpDM dm(std::move(design), loadOptions<DmpOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::dmp
