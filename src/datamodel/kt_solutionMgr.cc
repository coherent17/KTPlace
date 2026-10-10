// @file kt_solution.cc

#include "datamodel/kt_solutionMgr.h"

#include <algorithm>
#include <limits>

namespace ktplace {

double netlistHPWL(const Graph &graph, const std::vector<double> &x, const std::vector<double> &y) {
    double total = 0.0;
    const std::size_t nv = graph.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &net = graph.getVertex(v);
        if (net.type != VertexType::Net || net.inEdges.size() < 2) {
            continue;
        }
        double x0 = std::numeric_limits<double>::max();
        double x1 = -std::numeric_limits<double>::max();
        double y0 = std::numeric_limits<double>::max();
        double y1 = -std::numeric_limits<double>::max();
        for (const std::size_t eid : net.inEdges) {
            const Edge &e = graph.getEdge(eid);
            const Vertex &pin = graph.getVertex(e.source);
            if (pin.type != VertexType::Cell) {
                continue;
            }
            // The cell's far edge, not just the pin: the offset says where the
            // pin sits on the cell, and the wire ends at the cell's other side.
            x0 = std::min(x0, x[e.source] + e.offsetX);
            x1 = std::max(x1, x[e.source] + e.offsetX + pin.width);
            y0 = std::min(y0, y[e.source] + e.offsetY);
            y1 = std::max(y1, y[e.source] + e.offsetY + pin.height);
        }
        total += (x1 - x0) + (y1 - y0);
    }
    return total;
}

solutionMgr solutionMgr::fromGraph(const Graph &graph) {
    solutionMgr s(graph.getNumVertices());
    const std::size_t nv = graph.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph.getVertex(v);
        s.x_[v] = vert.x;
        s.y_[v] = vert.y;
    }
    return s;
}

void solutionMgr::commitTo(Graph &graph) const {
    const std::size_t nv = graph.getNumVertices();
    const std::size_t n = std::min(nv, x_.size());
    for (std::size_t v = 0; v < n; ++v) {
        Vertex &vert = graph.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        vert.x = x_[v];
        vert.y = y_[v];
    }
}

}  // namespace ktplace
