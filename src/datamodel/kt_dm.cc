// @file kt_dm.cc// Implementation of PlacementDB using PIMPL pattern with kt_graph


#include "datamodel/kt_dm.h"

#include "datamodel/kt_graph.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <fmt/format.h>
#include <limits>
#include <stdexcept>

// oneTBB - parallel stats
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace ktplace {

// Forward declaration for RowData
struct SubrowData {
    double originX = 0.0;
    double numSites = 0.0;
};

class RowData {
public:
    double coordinate = 0.0;
    double height = 0.0;
    double sitewidth = 0.0;
    double sitespacing = 0.0;
    std::vector<SubrowData> subrows;
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
                                double sitespacing) {
    RowData row;
    row.coordinate = coordinate;
    row.height = height;
    row.sitewidth = sitewidth;
    row.sitespacing = sitespacing;

    const std::size_t id = pImpl->rows.size();
    pImpl->rows.push_back(row);
    return id;
}

std::size_t PlacementDB::addSubrow(std::size_t rowId, double originX, double numSites) {
    if (rowId >= pImpl->rows.size()) {
        return static_cast<std::size_t>(-1);
    }
    pImpl->rows[rowId].subrows.push_back(SubrowData{originX, numSites});
    return pImpl->rows[rowId].subrows.size() - 1;
}

std::size_t PlacementDB::getNumRows() const {
    return pImpl->rows.size();
}

std::vector<PlacementDB::RowInfo> PlacementDB::getRows() const {
    std::vector<RowInfo> out;
    out.reserve(pImpl->rows.size());
    for (const RowData &r : pImpl->rows) {
        RowInfo ri;
        ri.coordinate = r.coordinate;
        ri.height = r.height;
        ri.sitewidth = r.sitewidth;
        ri.sitespacing = r.sitespacing;
        ri.subrows.reserve(r.subrows.size());
        for (const SubrowData &sr : r.subrows) {
            ri.subrows.push_back(SubrowInfo{sr.originX, sr.numSites});
        }
        out.push_back(std::move(ri));
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

std::array<double, 4> placementDieBox(const PlacementDB &db) {
    const Graph &g = db.getGraph();
    // The fixed cells: the I/O pad ring bounds the die in a Bookshelf design.
    double lo[2] = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max()};
    double hi[2] = {-std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        if (!vert.isFixed && !vert.isTerminal) {
            continue;
        }
        lo[0] = std::min(lo[0], vert.x);
        lo[1] = std::min(lo[1], vert.y);
        hi[0] = std::max(hi[0], vert.x + std::max(vert.width, 1.0));
        hi[1] = std::max(hi[1], vert.y + std::max(vert.height, 1.0));
    }
    std::array<double, 4> box{lo[0], lo[1], hi[0], hi[1]};
    const bool haveFixed = (box[2] > box[0]) && (box[3] > box[1]);

    // A declared die area, when the format carries one and it contains every
    // fixed cell. A declared area that excludes a fixed cell is not describing
    // the same die the pads describe, so it is not trusted.
    const auto da = db.getDieArea();
    if (da.second.first > da.first.first && da.second.second > da.first.second) {
        bool contains = true;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell || !vert.isFixed) {
                continue;
            }
            if (vert.x < da.first.first - 1.0 || vert.y < da.first.second - 1.0 ||
                vert.x + vert.width > da.second.first + 1.0 ||
                vert.y + vert.height > da.second.second + 1.0) {
                contains = false;
                break;
            }
        }
        if (contains) {
            box = {da.first.first, da.first.second, da.second.first, da.second.second};
        }
    }

    // The rows, unioned in. A cell in a row is legal by definition of a row, and
    // for adaptec3 the rows reach below the fixed cells, so a box without them
    // excludes a row the legalizer is right to have used.
    double rlo = std::numeric_limits<double>::max(), rhi = -std::numeric_limits<double>::max();
    double blo = std::numeric_limits<double>::max(), bhi = -std::numeric_limits<double>::max();
    bool anyRow = false;
    for (const PlacementDB::RowInfo &ri : db.getRows()) {
        if (!(ri.pitch() > 0.0)) {
            continue;
        }
        rlo = std::min(rlo, ri.coordinate);
        rhi = std::max(rhi, ri.coordinate + ri.height);
        blo = std::min(blo, ri.xlo());
        bhi = std::max(bhi, ri.xhi());
        anyRow = true;
    }
    if (anyRow && (bhi > blo) && (rhi > rlo)) {
        if (!haveFixed || !((box[2] > box[0]) && (box[3] > box[1]))) {
            box = {blo, rlo, bhi, rhi};
        } else {
            box = {std::min(box[0], blo), std::min(box[1], rlo), std::max(box[2], bhi),
                   std::max(box[3], rhi)};
        }
    }

    if (!((box[2] > box[0]) && (box[3] > box[1]))) {
        // Genuinely nothing to go on: a 1x1 box keeps every division downstream
        // finite. The density grid in particular reports a utilisation of 1e13%
        // on a zero-area die, which poisons the look-ahead legalizer.
        return {0.0, 0.0, 1.0, 1.0};
    }
    return box;
}


PlacementDB::Utilisation PlacementDB::measureUtilisation() const {
    Utilisation u;
    const Graph &g = getGraph();
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        const double a = vert.width * vert.height;
        // A terminal is fixed area, not absent area. In the ISPD 2005 Bookshelf
        // suites the macros *are* the terminals, so skipping them reports adaptec1
        // as having no macros at all and understates the demand on the rows.
        if (vert.isFixed || vert.isTerminal) {
            u.fixedArea += a;
        } else {
            u.cellArea += a;
            ++u.cells;
        }
    }
    for (const RowInfo &r : getRows()) {
        if (!(r.pitch() > 0.0) || !(r.height > 0.0)) {
            continue;
        }
        u.rowHeight = std::max(u.rowHeight, r.height);
        for (const SubrowInfo &si : r.subrows) {
            if (si.xhi(r.pitch()) > si.xlo()) {
                u.rowArea += (si.xhi(r.pitch()) - si.xlo()) * r.height;
            }
        }
    }
    // Cells that cannot fit a single row: the other way a design can be
    // unplaceable at any density.
    if (u.rowHeight > 0.0) {
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type == VertexType::Cell && !vert.isFixed && !vert.isTerminal &&
                vert.height > u.rowHeight * 1.5) {
                ++u.multiRow;
            }
        }
    }
    return u;
}

void PlacementDB::reportUtilisation() const {
    const Utilisation u = measureUtilisation();
    // Movable demand against the rows, which decides whether the design fits. The
    // fixed cells already occupy the rows rather than compete for them, so
    // charging their area here double-counts it (on adaptec1, 58% reads as 89%).
    const double util = u.rowArea > 0.0 ? 100.0 * u.cellArea / u.rowArea : 0.0;
    const double withFixed = u.rowArea > 0.0 ? 100.0 * (u.cellArea + u.fixedArea) / u.rowArea : 0.0;
    ktReportTable t("Design utilisation (before placement)");
    t.setHeaders({"measure", "value"});
    t.addRow({"movable cell area", fmt::format("{:.6e}", u.cellArea)});
    t.addRow({"fixed cell area", fmt::format("{:.6e}", u.fixedArea)});
    t.addRow({"row (placeable) area", fmt::format("{:.6e}", u.rowArea)});
    t.addRow({"utilisation (movable / rows)", fmt::format("{:.2}%", util)});
    t.addRow({"utilisation (incl. fixed cells)", fmt::format("{:.2}%", withFixed)});
    t.addRow({"movable cells", fmt::format("{}", u.cells)});
    if (u.rowHeight > 0.0) {
        t.addRow({"row height", fmt::format("{:.3}", u.rowHeight)});
        t.addRow({"cells taller than one row", fmt::format("{}", u.multiRow)});
    }
    t.emit();
    if (u.rowArea > 0.0 && util > 100.0) {
        ktlog.warning(
            "the design needs {:.6e} of movable cell area but only {:.6e} of row is placeable, "
            "so it is {:.1f}% full. No legal placement exists for this input: the cells do not "
            "fit, however the placer is retried.",
            u.cellArea, u.rowArea, util);
    }
    if (u.multiRow > 0) {
        ktlog.warning(
            "{} cell(s) are taller than one row (row height {:.3}) and the legalizer only "
            "places into single rows, so those cells will be left unplaced and the result will "
            "not be legal. Legalizing this design needs a multi-height legalizer, which this "
            "build does not have.",
            u.multiRow, u.rowHeight);
    }
}

void PlacementDB::report() const {
    const auto [numCells, numNets] = getStats();
    // What placement did the design ship with? Worth logging: if it is missing or
    // degenerate every placer silently falls back to its own seed, which looks
    // like a placer bug and is not one.
    std::size_t moved = 0;
    double lo = 1e300;
    double hi = -1e300;
    double loY = 1e300;
    double hiY = -1e300;
    const Graph &g = getGraph();
    for (std::size_t v = 0; v < g.getNumVertices(); ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        lo = std::min(lo, vert.x);
        hi = std::max(hi, vert.x);
        loY = std::min(loY, vert.y);
        hiY = std::max(hiY, vert.y);
        moved += (vert.x != 0.0 || vert.y != 0.0) ? 1 : 0;
    }
    ktlog.echo(
        "Loaded placement: {}/{} cells carry a position, bbox x[{:.1f},{:.1f}] y[{:.1f},{:.1f}]",
        moved, g.getNumVertices(), lo, hi, loY, hiY);
    ktlog.echo("Loaded: {} cells ({} terminals), {} nets, {} pins, {} rows", numCells,
               getNumTerminals(), numNets, getNumPins(), getNumRows());
}
}  // namespace ktplace
