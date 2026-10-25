// @file kt_solutionMgr.cc

#include "datamodel/kt_solutionMgr.h"

#include "datamodel/kt_hpwl.h"

#include <utility>

namespace ktplace {

double netlistHPWL(const Graph &graph, const std::vector<double> &x, const std::vector<double> &y) {
    return netHpwl(
        graph.getNumNets(),
        [&](std::size_t n) -> const std::vector<std::size_t> & {
            return graph.getNetPins(n);
        },
        [&](std::size_t pinId) {
            const Pin &pin = graph.getPin(pinId);
            return std::pair{x[pin.cellId] + pin.offsetX, y[pin.cellId] + pin.offsetY};
        });
}

solutionMgr solutionMgr::fromGraph(const Graph &graph) {
    solutionMgr s(graph.getNumCells());
    const std::size_t nv = graph.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph.getCell(v);
        s.xPos[v] = vert.x;
        s.yPos[v] = vert.y;
    }
    return s;
}

void solutionMgr::commitTo(Graph &graph) const {
    const std::size_t nv = graph.getNumCells();
    const std::size_t n = std::min(nv, xPos.size());
    for (std::size_t v = 0; v < n; ++v) {
        Vertex &vert = graph.getCell(v);
        vert.x = xPos[v];
        vert.y = yPos[v];
    }
}

}  // namespace ktplace
