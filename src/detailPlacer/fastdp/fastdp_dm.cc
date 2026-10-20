#include "detailPlacer/fastdp/fastdp_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "detailPlacer/fastdp/fastdp_placer.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

namespace ktplace::fastdp {

namespace {

void copyCells(const ktplace::Graph &graph, double macroHeight, std::vector<Cell> &out) {
    out.resize(graph.getNumCells());
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &cell = graph.getCell(v);
        Cell &c = out[v];
        c.x = cell.x;
        c.y = cell.y;
        c.width = cell.width;
        c.height = cell.height;
        c.isFixed = cell.isFixed || cell.height > macroHeight;
        c.isTerminal = cell.isTerminal;
        c.region = cell.regionId;
    }
}

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

void reportRun(const FastdpResult &r) {
    ktReportTable t("Detailed placement (FastDP)");
    t.setHeaders({"metric", "value"});
    t.addRow({"global swaps", std::format("{} ({:.3f}s)", r.globalSwaps, r.globalSeconds)});
    t.addRow({"vertical swaps", std::format("{} ({:.3f}s)", r.verticalSwaps, r.verticalSeconds)});
    t.addRow({"reorder moves", std::format("{} ({:.3f}s)", r.reorderMoves, r.reorderSeconds)});
    t.addRow({"cluster moves", std::format("{} ({:.3f}s)", r.clusterMoves, r.clusterSeconds)});
    t.addRow({"passes rolled back", std::format("{}", r.rolledBack)});
    t.addRow({"HPWL before", std::format("{:.6e}", r.hpwlBefore)});
    t.addRow({"HPWL after", std::format("{:.6e}", r.hpwlAfter)});
    t.addRow(
        {"HPWL change", std::format("{:+.2f}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                                    (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"seconds", std::format("{:.3f}", r.seconds)});
    t.addRow({"overlapping pairs", std::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", std::format("{}", r.offRow)});
    t.addRow({"cells off site", std::format("{}", r.offSite)});
    t.addRow({"cells over macro", std::format("{}", r.overFixed)});
    t.emit();
    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
}

}  // namespace

double netlistHPWL(const std::vector<Cell> &, const std::vector<Net> &nets,
                   const std::vector<Pin> &pins, const std::vector<double> &xs,
                   const std::vector<double> &ys) {
    double total = 0.0;
    for (const Net &net : nets) {
        if (net.pins.size() < 2) {
            continue;
        }
        double xlo = std::numeric_limits<double>::max();
        double xhi = std::numeric_limits<double>::lowest();
        double ylo = std::numeric_limits<double>::max();
        double yhi = std::numeric_limits<double>::lowest();
        for (const std::size_t pinId : net.pins) {
            const Pin &pin = pins.at(pinId);
            const double px = xs.at(pin.cellId) + pin.offsetX;
            const double py = ys.at(pin.cellId) + pin.offsetY;
            xlo = std::min(xlo, px);
            xhi = std::max(xhi, px);
            ylo = std::min(ylo, py);
            yhi = std::max(yhi, py);
        }
        total += (xhi - xlo) + (yhi - ylo);
    }
    return total;
}

fastdpDM::fastdpDM(Design design, FastdpOptions options)
    : d(std::move(design)), opts(std::move(options)) {}

FastdpResult fastdpDM::run() {
    FastDetailedPlacer placer(d);
    const FastdpResult result = placer.place(opts);
    reportRun(result);
    return result;
}

fastdpSolution fastdpDM::solution() const {
    fastdpSolution out;
    out.xs.reserve(d.cells.size());
    out.ys.reserve(d.cells.size());
    for (const Cell &cell : d.cells) {
        out.xs.push_back(cell.x);
        out.ys.push_back(cell.y);
    }
    return out;
}

fastdpDM buildFastdpDM(const ktDM &db) {
    Design design;
    copyCells(db.getGraph(), db.macroHeight(), design.cells);
    copyNets(db.getGraph(), design.pins, design.nets);
    copyRows(db.getRows(), db.fencedSubrows(), design.rows);
    design.die = placementDieBox(db.die(), db.getGraph());

    const StageCounts counts = countStage(design);
    fastdpDM dm(std::move(design), loadOptions<FastdpOptions>());
    reportStageBuild(counts, db.checksum(), dm.options());
    return dm;
}

}  // namespace ktplace::fastdp
