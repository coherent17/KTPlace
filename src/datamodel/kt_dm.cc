/**
 * @file kt_dm.cc
 * @brief Implementation of PlacementDB using PIMPL pattern with kt_graph
 */

#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"
#include <algorithm>
#include <stdexcept>

// oneTBB - parallel stats
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/blocked_range.h>

namespace ktplace {

// Forward declaration for RowData
class RowData {
public:
    double coordinate = 0.0;
    double height = 0.0;
    double sitewidth = 0.0;
    double sitespacing = 0.0;
    double numSites = 0.0;
    double originX = 0.0;
};

// PIMPL implementation
class PlacementDB::Impl {
public:
    // Graph structure (replaces separate _cells, _nets, _pins vectors)
    Graph graph;

    // Rows (layout information)
    std::vector<RowData> rows;

    // Die area
    double dieXMin = 0.0;
    double dieYMin = 0.0;
    double dieXMax = 0.0;
    double dieYMax = 0.0;
};

// PlacementDB implementation

PlacementDB::PlacementDB() : pImpl(std::make_unique<Impl>()) {}

PlacementDB::~PlacementDB() = default;

PlacementDB::PlacementDB(PlacementDB &&) noexcept = default;
PlacementDB &PlacementDB::operator=(PlacementDB &&) noexcept = default;

std::size_t PlacementDB::addCell(const std::string &name, double width, double height,
                                 bool isTerminal) {
    std::size_t id = pImpl->graph.addVertex(VertexType::Cell, name);
    Vertex &v = pImpl->graph.getVertex(id);
    v.width = width;
    v.height = height;
    v.isTerminal = isTerminal;
    return id;
}

bool PlacementDB::hasCell(const std::string &name) const {
    return pImpl->graph.hasVertex(name) &&
           pImpl->graph.getVertexType(pImpl->graph.getVertexId(name)) == VertexType::Cell;
}

std::size_t PlacementDB::getCellId(const std::string &name) const {
    std::size_t id = pImpl->graph.getVertexId(name);
    if (pImpl->graph.getVertexType(id) != VertexType::Cell) {
        throw std::runtime_error(name + " is not a cell");
    }
    return id;
}

std::size_t PlacementDB::getNumCells() const {
    return pImpl->graph.getNumVertices(VertexType::Cell);
}

std::size_t PlacementDB::getNumTerminals() const {
    const Graph &g = pImpl->graph;
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, g.getNumVertices()), std::size_t(0),
        [&](const tbb::blocked_range<std::size_t> &r, std::size_t acc) {
            for (std::size_t i = r.begin(); i != r.end(); ++i) {
                const Vertex &v = g.getVertex(i);
                if (v.type == VertexType::Cell && v.isTerminal) {
                    ++acc;
                }
            }
            return acc;
        },
        [](std::size_t a, std::size_t b) {
            return a + b;
        });
}

std::size_t PlacementDB::addNet(const std::string &name, double weight) {
    std::size_t id = pImpl->graph.addVertex(VertexType::Net, name);
    Vertex &v = pImpl->graph.getVertex(id);
    v.weight = weight;
    return id;
}

bool PlacementDB::hasNet(const std::string &name) const {
    return pImpl->graph.hasVertex(name) &&
           pImpl->graph.getVertexType(pImpl->graph.getVertexId(name)) == VertexType::Net;
}

std::size_t PlacementDB::getNetId(const std::string &name) const {
    std::size_t id = pImpl->graph.getVertexId(name);
    if (pImpl->graph.getVertexType(id) != VertexType::Net) {
        throw std::runtime_error(name + " is not a net");
    }
    return id;
}

std::size_t PlacementDB::getNumNets() const {
    return pImpl->graph.getNumVertices(VertexType::Net);
}

std::size_t PlacementDB::addPin(const std::string &cellName, const std::string &netName,
                                double offsetX, double offsetY, bool isInput) {
    std::size_t cellId = getCellId(cellName);
    std::size_t netId = getNetId(netName);

    PinDirection dir = isInput ? PinDirection::Input : PinDirection::Output;
    std::size_t edgeId = pImpl->graph.addEdge(cellId, netId, dir);

    Edge &e = pImpl->graph.getEdge(edgeId);
    e.offsetX = offsetX;
    e.offsetY = offsetY;

    return edgeId;
}

std::size_t PlacementDB::getNumPins() const {
    return pImpl->graph.getNumEdges();
}

void PlacementDB::setCellPosition(std::size_t cellId, double x, double y) {
    Vertex &v = pImpl->graph.getVertex(cellId);
    if (v.type != VertexType::Cell) {
        throw std::runtime_error("Invalid cell ID");
    }
    v.x = x;
    v.y = y;
}

void PlacementDB::setCellPosition(const std::string &cellName, double x, double y) {
    setCellPosition(getCellId(cellName), x, y);
}

std::pair<double, double> PlacementDB::getCellPosition(std::size_t cellId) const {
    const Vertex &v = pImpl->graph.getVertex(cellId);
    if (v.type != VertexType::Cell) {
        throw std::runtime_error("Invalid cell ID");
    }
    return {v.x, v.y};
}

std::pair<double, double> PlacementDB::getCellPosition(const std::string &cellName) const {
    return getCellPosition(getCellId(cellName));
}

void PlacementDB::setCellFixed(std::size_t cellId, bool fixed) {
    Vertex &v = pImpl->graph.getVertex(cellId);
    if (v.type != VertexType::Cell) {
        throw std::runtime_error("Invalid cell ID");
    }
    v.isFixed = fixed;
}

void PlacementDB::setCellFixed(const std::string &cellName, bool fixed) {
    setCellFixed(getCellId(cellName), fixed);
}

bool PlacementDB::isCellFixed(std::size_t cellId) const {
    const Vertex &v = pImpl->graph.getVertex(cellId);
    if (v.type != VertexType::Cell) {
        throw std::runtime_error("Invalid cell ID");
    }
    return v.isFixed;
}

bool PlacementDB::isCellFixed(const std::string &cellName) const {
    return isCellFixed(getCellId(cellName));
}

std::size_t PlacementDB::addRow(double coordinate, double height, double sitewidth,
                                double sitespacing, double numSites, double originX) {
    RowData row;
    row.coordinate = coordinate;
    row.height = height;
    row.sitewidth = sitewidth;
    row.sitespacing = sitespacing;
    row.numSites = numSites;
    row.originX = originX;

    std::size_t id = pImpl->rows.size();
    pImpl->rows.push_back(row);
    return id;
}

std::size_t PlacementDB::getNumRows() const {
    return pImpl->rows.size();
}

std::vector<PlacementDB::RowInfo> PlacementDB::getRows() const {
    std::vector<RowInfo> out;
    out.reserve(pImpl->rows.size());
    for (const RowData &r : pImpl->rows) {
        out.push_back(
            RowInfo{r.coordinate, r.height, r.sitewidth, r.sitespacing, r.numSites, r.originX});
    }
    return out;
}

Graph &PlacementDB::getGraphImpl() {
    return pImpl->graph;
}

const Graph &PlacementDB::getGraphImpl() const {
    return pImpl->graph;
}

void PlacementDB::setDieArea(double xMin, double yMin, double xMax, double yMax) {
    pImpl->dieXMin = xMin;
    pImpl->dieYMin = yMin;
    pImpl->dieXMax = xMax;
    pImpl->dieYMax = yMax;
}

std::pair<std::pair<double, double>, std::pair<double, double>> PlacementDB::getDieArea() const {
    return {{pImpl->dieXMin, pImpl->dieYMin}, {pImpl->dieXMax, pImpl->dieYMax}};
}

const std::vector<std::size_t> &PlacementDB::getNetPins(std::size_t netId) const {
    const Vertex &v = pImpl->graph.getVertex(netId);
    if (v.type != VertexType::Net) {
        throw std::runtime_error("Invalid net ID");
    }
    return v.inEdges;  // Pins connect to nets as incoming edges
}

const std::vector<std::size_t> &PlacementDB::getCellPins(std::size_t cellId) const {
    const Vertex &v = pImpl->graph.getVertex(cellId);
    if (v.type != VertexType::Cell) {
        throw std::runtime_error("Invalid cell ID");
    }
    return v.outEdges;  // Pins connect from cells as outgoing edges
}

void PlacementDB::clear() {
    pImpl->graph.clear();
    pImpl->rows.clear();
    pImpl->dieXMin = pImpl->dieYMin = pImpl->dieXMax = pImpl->dieYMax = 0.0;
}

std::pair<std::size_t, std::size_t> PlacementDB::getStats() const {
    return {getNumCells(), getNumNets()};
}

}  // namespace ktplace
