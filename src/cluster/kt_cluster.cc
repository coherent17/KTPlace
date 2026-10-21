#include "cluster/kt_cluster.h"

#include "cluster/kt_leiden.h"
#include "cluster/kt_louvain.h"
#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_optionDb.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

using cluster::none;

class ktCluster::Impl {
public:
    explicit Impl(ClusterOptions o) : opts(std::move(o)) {}

    ClusterStats cluster(ktDM &db);
    void decluster(ktDM &db);
    void expand(const std::vector<float> &coarseX, const std::vector<float> &coarseY,
                std::vector<float> &x, std::vector<float> &y) const;

    ClusterOptions opts;
    Graph original;
    bool active = false;
    std::vector<std::uint32_t> coarseOf;
    std::vector<std::vector<std::uint32_t>> members;
    std::vector<std::uint32_t> clusterVertex;
    std::vector<std::uint32_t> clusterOfCell;
};

ClusterStats ktCluster::Impl::cluster(ktDM &db) {
    ScopedTimer timer("clustering", true);
    ClusterStats stats;
    const Graph &graph = db.getGraph();
    const std::size_t n = graph.getNumCells();
    stats.cells = n;

    double rowHeight = std::numeric_limits<double>::max();
    for (const RowInfo &row : db.getRows()) {
        if (row.height > 0.0 && !row.subrows.empty()) {
            rowHeight = std::min(rowHeight, row.height);
        }
    }
    std::vector<std::uint32_t> node(n, none);
    std::vector<std::uint32_t> cellOfNode;
    std::vector<double> area;
    std::vector<int> region;
    for (std::uint32_t v = 0; v < n; ++v) {
        const Vertex &cell = graph.getCell(v);
        if (cell.isFixed || cell.isTerminal || !(cell.height <= rowHeight * (1.0 + 1e-9)) ||
            !(cell.width > 0.0)) {
            continue;
        }
        node[v] = static_cast<std::uint32_t>(cellOfNode.size());
        cellOfNode.push_back(v);
        area.push_back(cell.width * cell.height);
        region.push_back(cell.regionId);
    }
    stats.clusterable = cellOfNode.size();
    if (cellOfNode.size() < 2) {
        stats.coarseCells = n;
        return stats;
    }

    std::vector<cluster::Edge> edges;
    std::vector<std::uint32_t> pinNodes;
    for (std::size_t net = 0; net < graph.getNumNets(); ++net) {
        const std::vector<std::size_t> &pins = graph.getNetPins(net);
        if (pins.size() < 2 || pins.size() > opts.maxNetSize) {
            continue;
        }
        pinNodes.clear();
        for (const std::size_t p : pins) {
            const std::uint32_t k = node[graph.getPin(p).cellId];
            if (k != none) {
                pinNodes.push_back(k);
            }
        }
        std::ranges::stable_sort(pinNodes);
        pinNodes.erase(std::unique(pinNodes.begin(), pinNodes.end()), pinNodes.end());
        const double w = graph.getNet(net).weight / static_cast<double>(pins.size() - 1);
        for (std::size_t a = 0; a < pinNodes.size(); ++a) {
            for (std::size_t b = a + 1; b < pinNodes.size(); ++b) {
                edges.push_back({pinNodes[a], pinNodes[b], w});
            }
        }
    }
    const double meanArea =
        std::accumulate(area.begin(), area.end(), 0.0) / static_cast<double>(area.size());
    const double areaCap = opts.areaLimit * opts.cellsPerCluster * meanArea;
    cluster::Level level = cluster::buildLevel(
        area, region, std::vector<double>(cellOfNode.size(), 0.0), std::move(edges));
    const cluster::Communities found = opts.algorithm == "leiden"
                                           ? cluster::leiden(std::move(level), opts, areaCap)
                                           : cluster::louvain(std::move(level), opts, areaCap);
    stats.levels = found.levels;

    std::vector<double> clusterArea(found.count, 0.0);
    std::vector<double> sumX(found.count, 0.0);
    std::vector<double> sumY(found.count, 0.0);
    members.assign(found.count, {});
    for (std::uint32_t k = 0; k < cellOfNode.size(); ++k) {
        const std::uint32_t c = found.ofNode[k];
        const Vertex &cell = graph.getCell(cellOfNode[k]);
        members[c].push_back(cellOfNode[k]);
        clusterArea[c] += area[k];
        sumX[c] += area[k] * cell.x;
        sumY[c] += area[k] * cell.y;
    }
    clusterOfCell.assign(n, none);
    for (std::uint32_t k = 0; k < cellOfNode.size(); ++k) {
        if (members[found.ofNode[k]].size() > 1) {
            clusterOfCell[cellOfNode[k]] = found.ofNode[k];
        }
    }

    Graph coarse;
    coarseOf.assign(n, none);
    clusterVertex.assign(found.count, none);
    for (std::uint32_t v = 0; v < n; ++v) {
        const Vertex &cell = graph.getCell(v);
        const std::uint32_t c = clusterOfCell[v];
        if (c == none) {
            const std::size_t id = coarse.addCell(cell.name);
            Vertex &copy = coarse.getCell(id);
            copy = cell;
            copy.id = id;
            coarseOf[v] = static_cast<std::uint32_t>(id);
            continue;
        }
        if (clusterVertex[c] == none) {
            std::string name = std::format("kt_cluster_{}", c);
            while (graph.hasCell(name)) {
                name += '_';
            }
            const std::size_t id = coarse.addCell(name);
            Vertex &box = coarse.getCell(id);
            box.height = rowHeight;
            box.width = clusterArea[c] / rowHeight;
            box.x = sumX[c] / clusterArea[c];
            box.y = sumY[c] / clusterArea[c];
            box.regionId = cell.regionId;
            clusterVertex[c] = static_cast<std::uint32_t>(id);
            stats.largestArea = std::max(stats.largestArea, clusterArea[c]);
            ++stats.clusters;
        }
        coarseOf[v] = clusterVertex[c];
    }
    for (std::size_t net = 0; net < graph.getNumNets(); ++net) {
        (void)coarse.addNet(graph.getNet(net).name, graph.getNet(net).weight);
    }
    for (std::size_t p = 0; p < graph.getNumPins(); ++p) {
        const Pin &pin = graph.getPin(p);
        const std::uint32_t target = coarseOf[pin.cellId];
        const bool inCluster = clusterOfCell[pin.cellId] != none;
        const Vertex &box = coarse.getCell(target);
        (void)coarse.addPin(target, pin.netId, pin.role, inCluster ? 0.5 * box.width : pin.offsetX,
                            inCluster ? 0.5 * box.height : pin.offsetY);
    }
    stats.coarseCells = coarse.getNumCells();

    original = std::move(db.getGraph());
    db.getGraph() = std::move(coarse);
    active = true;
    stats.seconds = timer.elapsedSeconds();

    ktReportTable t(opts.algorithm == "leiden" ? "Clustering (Leiden)" : "Clustering (Louvain)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells", std::to_string(stats.cells)});
    t.addRow({"clusterable cells", std::to_string(stats.clusterable)});
    t.addRow({"clusters (2+ cells)", std::to_string(stats.clusters)});
    t.addRow({"cells after clustering", std::to_string(stats.coarseCells)});
    t.addRow({"levels", std::to_string(stats.levels)});
    t.addRow({"area cap", std::format("{:.6g}", areaCap)});
    t.addRow({"largest cluster area", std::format("{:.6g}", stats.largestArea)});
    t.addRow({"seconds", std::format("{:.3f}", stats.seconds)});
    t.emit();
    reportOptions(opts);
    return stats;
}

void ktCluster::Impl::expand(const std::vector<float> &coarseX, const std::vector<float> &coarseY,
                             std::vector<float> &x, std::vector<float> &y) const {
    const std::size_t n = coarseOf.size();
    x.resize(n);
    y.resize(n);
    for (std::size_t v = 0; v < n; ++v) {
        x[v] = coarseX[coarseOf[v]];
        y[v] = coarseY[coarseOf[v]];
    }
    for (std::size_t c = 0; c < members.size(); ++c) {
        if (clusterVertex[c] == none) {
            continue;
        }
        float cursor = coarseX[clusterVertex[c]];
        for (const std::uint32_t v : members[c]) {
            x[v] = cursor;
            cursor += static_cast<float>(original.getCell(v).width);
        }
    }
}

void ktCluster::Impl::decluster(ktDM &db) {
    if (!active) {
        return;
    }
    Graph coarse = std::move(db.getGraph());
    db.getGraph() = std::move(original);
    active = false;
    Graph &graph = db.getGraph();
    const std::size_t n = graph.getNumCells();
    for (std::uint32_t v = 0; v < n; ++v) {
        if (clusterOfCell[v] == none) {
            const Vertex &placed = coarse.getCell(coarseOf[v]);
            graph.getCell(v).x = placed.x;
            graph.getCell(v).y = placed.y;
        }
    }
    const auto pinX = [&](std::size_t pinId) {
        const Pin &pin = graph.getPin(pinId);
        const Vertex &at = coarse.getCell(coarseOf[pin.cellId]);
        return clusterOfCell[pin.cellId] == none ? at.x + pin.offsetX : at.x + 0.5 * at.width;
    };
    for (std::size_t c = 0; c < members.size(); ++c) {
        if (clusterVertex[c] == none) {
            continue;
        }
        const Vertex &box = coarse.getCell(clusterVertex[c]);
        std::vector<std::pair<double, std::uint32_t>> order;
        for (const std::uint32_t v : members[c]) {
            double sum = 0.0;
            std::size_t count = 0;
            for (const std::size_t p : graph.getCellPins(v)) {
                for (const std::size_t q : graph.getNetPins(graph.getPin(p).netId)) {
                    const std::size_t other = graph.getPin(q).cellId;
                    if (clusterOfCell[other] != c) {
                        sum += pinX(q);
                        ++count;
                    }
                }
            }
            order.emplace_back(
                count > 0 ? sum / static_cast<double>(count) : box.x + 0.5 * box.width, v);
        }
        std::ranges::stable_sort(order);
        double cursor = box.x;
        for (const auto &[key, v] : order) {
            Vertex &cell = graph.getCell(v);
            cell.x = cursor;
            cell.y = box.y;
            cursor += cell.width;
        }
    }
}

ktCluster::ktCluster(ClusterOptions options) : pImpl(std::make_unique<Impl>(std::move(options))) {}

ktCluster::~ktCluster() = default;

ktCluster::ktCluster(ktCluster &&) noexcept = default;

ktCluster &ktCluster::operator=(ktCluster &&) noexcept = default;

ClusterStats ktCluster::cluster(ktDM &db) {
    return pImpl->cluster(db);
}

void ktCluster::decluster(ktDM &db) {
    pImpl->decluster(db);
}

void ktCluster::expand(const std::vector<float> &coarseX, const std::vector<float> &coarseY,
                       std::vector<float> &x, std::vector<float> &y) const {
    pImpl->expand(coarseX, coarseY, x, y);
}

bool ktCluster::clustered() const {
    return pImpl->active;
}

const std::vector<std::uint32_t> &ktCluster::clusterOf() const {
    return pImpl->coarseOf;
}

}  // namespace ktplace
