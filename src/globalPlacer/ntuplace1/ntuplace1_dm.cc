#include "globalPlacer/ntuplace1/ntuplace1_dm.h"

#include "datamodel/kt_die.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_stageReport.h"
#include "globalPlacer/ntuplace1/ntuplace1_placer.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ktplace::ntuplace1 {

namespace {

constexpr std::uint32_t noIndex = 0xFFFFFFFFu;

std::size_t collectCells(const ktplace::Graph &graph, Design &design) {
    for (std::size_t v = 0; v < graph.getNumCells(); ++v) {
        const ktplace::Vertex &vert = graph.getCell(v);
        design.vertexX.push_back(vert.x);
        design.vertexY.push_back(vert.y);
        if (vert.isFixed || vert.isTerminal) {
            if (vert.width > 0.0 && vert.height > 0.0) {
                design.fixedRects.push_back(
                    {vert.x, vert.y, vert.x + vert.width, vert.y + vert.height});
            }
            continue;
        }
        Cells &cells = design.cells;
        cells.id.push_back(v);
        cells.width.push_back(vert.width);
        cells.height.push_back(vert.height);
        cells.area.push_back(vert.width * vert.height);
        cells.region.push_back(vert.regionId);
        cells.x.push_back(vert.x);
        cells.y.push_back(vert.y);
    }
    return design.cells.size();
}

void collectHyperNets(const ktplace::Graph &graph, Design &design) {
    const Cells &cells = design.cells;
    std::vector<std::uint32_t> indexOf(graph.getNumCells(), noIndex);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        indexOf[cells.id[i]] = static_cast<std::uint32_t>(i);
    }
    design.cellNets.assign(cells.size(), {});
    std::vector<std::size_t> lastNet(cells.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t v = 0; v < graph.getNumNets(); ++v) {
        HyperNet n;
        for (const std::size_t pinId : graph.getNetPins(v)) {
            const ktplace::Pin &e = graph.getPin(pinId);
            if (indexOf[e.cellId] != noIndex) {
                const std::uint32_t c = indexOf[e.cellId];
                if (lastNet[c] != v) {
                    lastNet[c] = v;
                    n.cells.push_back(c);
                }
                continue;
            }
            const ktplace::Vertex &pin = graph.getCell(e.cellId);
            const double px = pin.x + e.offsetX;
            const double py = pin.y + e.offsetY;
            if (n.fixedPins == 0) {
                n.fx0 = n.fx1 = px;
                n.fy0 = n.fy1 = py;
            } else {
                n.fx0 = std::min(n.fx0, px);
                n.fx1 = std::max(n.fx1, px);
                n.fy0 = std::min(n.fy0, py);
                n.fy1 = std::max(n.fy1, py);
            }
            ++n.fixedPins;
        }
        if (n.cells.empty() || n.cells.size() + n.fixedPins < 2) {
            continue;
        }
        const auto id = static_cast<std::uint32_t>(design.hyperNets.size());
        for (const std::uint32_t c : n.cells) {
            design.cellNets[c].push_back(id);
        }
        design.hyperNets.push_back(std::move(n));
    }
}

void collectRows(const std::vector<ktplace::RowInfo> &rows, Design &design) {
    design.rowBox = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                     -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    for (const ktplace::RowInfo &r : rows) {
        for (const ktplace::SubrowInfo &s : r.subrows) {
            const AreaRect rr{s.getXlo(), r.coordinate, s.getXhi(r.getPitch()),
                              r.coordinate + r.height};
            if (rr.x1 <= rr.x0) {
                continue;
            }
            design.rowRects.push_back(rr);
            design.rowBox[0] = std::min(design.rowBox[0], rr.x0);
            design.rowBox[1] = std::min(design.rowBox[1], rr.y0);
            design.rowBox[2] = std::max(design.rowBox[2], rr.x1);
            design.rowBox[3] = std::max(design.rowBox[3], rr.y1);
        }
    }
    if (design.rowRects.empty()) {
        design.rowBox = design.die;
    }
    std::stable_sort(design.rowRects.begin(), design.rowRects.end(),
                     [](const AreaRect &a, const AreaRect &b) {
                         return a.y0 < b.y0;
                     });
    design.maxRowH = 0.0;
    for (const AreaRect &r : design.rowRects) {
        design.maxRowH = std::max(design.maxRowH, r.y1 - r.y0);
    }
}

}  // namespace

ntuplace1DM::ntuplace1DM(Design design, Ntuplace1Options options)
    : d(std::move(design)), opts(std::move(options)) {}

void ntuplace1DM::run() {
    RatioPlacer placer(d);
    placer.place(opts);
}

ntuplace1Solution ntuplace1DM::getSolution() const {
    ntuplace1Solution out;
    out.xs = d.vertexX;
    out.ys = d.vertexY;
    for (std::size_t i = 0; i < d.cells.size(); ++i) {
        out.xs[d.cells.id[i]] = d.cells.x[i];
        out.ys[d.cells.id[i]] = d.cells.y[i];
    }
    return out;
}

ntuplace1DM buildNtuplace1DM(const ktDM &db) {
    Design design;
    const ktplace::Graph &graph = db.getGraph();
    const std::size_t movable = collectCells(graph, design);
    collectHyperNets(graph, design);
    design.die = getPlacementDieBox(db.getDie(), graph);
    collectRows(db.getRows(), design);
    design.fences = db.getConstraints();

    const StageCounts counts{graph.getNumCells(), movable, graph.getNumNets(), graph.getNumPins(),
                             db.getRows().size()};
    ntuplace1DM dm(std::move(design), loadOptions<Ntuplace1Options>());
    reportStageBuild(counts, db.getChecksum(), dm.getOptions());
    return dm;
}

}  // namespace ktplace::ntuplace1
