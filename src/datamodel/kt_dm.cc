// @file kt_dm.cc// Implementation of ktDM using PIMPL pattern with kt_graph


#include "datamodel/kt_dm.h"

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_solutionMgr.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

// oneTBB - parallel stats
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace ktplace {

// ktDM implementation

std::size_t ktDM::addCell(const std::string &name, double width, double height, bool isTerminal) {
    const std::size_t id = graph.addCell(name);
    Vertex &v = graph.getCell(id);
    v.width = width;
    v.height = height;
    v.isTerminal = isTerminal;
    return id;
}

void ktDM::markIoPin(const std::string &name) {
    graph.getCell(graph.getCellId(name)).isIoPin = true;
}

bool ktDM::hasCell(const std::string &name) const {
    return graph.hasCell(name);
}

std::size_t ktDM::getCellId(const std::string &name) const {
    return graph.getCellId(name);
}

std::size_t ktDM::getNumCells() const {
    return graph.getNumCells();
}

std::size_t ktDM::getNumTerminals() const {
    const Graph &g = graph;
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, g.getNumCells()), std::size_t(0),
        [&](const tbb::blocked_range<std::size_t> &r, std::size_t acc) {
            for (std::size_t i = r.begin(); i < r.end(); ++i) {
                if (g.getCell(i).isTerminal) {
                    ++acc;
                }
            }
            return acc;
        },
        [](std::size_t a, std::size_t b) {
            return a + b;
        });
}

std::size_t ktDM::addNet(const std::string &name, double weight) {
    return graph.addNet(name, weight);
}

bool ktDM::hasNet(const std::string &name) const {
    return graph.hasNet(name);
}

std::size_t ktDM::getNetId(const std::string &name) const {
    return graph.getNetId(name);
}

std::size_t ktDM::getNumNets() const {
    return graph.getNumNets();
}

std::size_t ktDM::addPin(const std::string &cellName, const std::string &netName, double offsetX,
                         double offsetY, bool isInput) {
    const std::size_t cellId = getCellId(cellName);
    const std::size_t netId = getNetId(netName);
    return graph.addPin(cellId, netId, isInput ? PinRole::Receiver : PinRole::Driver, offsetX,
                        offsetY);
}

std::size_t ktDM::getNumPins() const {
    return graph.getNumPins();
}

void ktDM::setCellPosition(std::size_t cellId, double x, double y) {
    Vertex &v = graph.getCell(cellId);
    v.x = x;
    v.y = y;
}

void ktDM::setCellPosition(const std::string &cellName, double x, double y) {
    setCellPosition(getCellId(cellName), x, y);
}

std::pair<double, double> ktDM::getCellPosition(std::size_t cellId) const {
    const Vertex &v = graph.getCell(cellId);
    return {v.x, v.y};
}

std::pair<double, double> ktDM::getCellPosition(const std::string &cellName) const {
    return getCellPosition(getCellId(cellName));
}

void ktDM::setCellFixed(std::size_t cellId, bool fixed) {
    graph.getCell(cellId).isFixed = fixed;
}

void ktDM::setCellFixed(const std::string &cellName, bool fixed) {
    setCellFixed(getCellId(cellName), fixed);
}

bool ktDM::isCellFixed(std::size_t cellId) const {
    return graph.getCell(cellId).isFixed;
}

bool ktDM::isCellFixed(const std::string &cellName) const {
    return isCellFixed(getCellId(cellName));
}

std::size_t ktDM::addRow(double coordinate, double height, double sitewidth, double sitespacing) {
    return dieInfo.addRow(coordinate, height, sitewidth, sitespacing);
}

std::size_t ktDM::addSubrow(std::size_t rowId, double originX, double numSites) {
    return dieInfo.addSubrow(rowId, originX, numSites);
}

void ktDM::clear() {
    graph.clear();
    dieInfo.clear();
}

std::pair<std::size_t, std::size_t> ktDM::getStats() const {
    return {getNumCells(), getNumNets()};
}

std::size_t ktDM::getNumRows() const {
    return dieInfo.getNumRows();
}

const std::vector<RowInfo> &ktDM::getRows() const {
    return dieInfo.getRows();
}

void ktDM::setDieArea(const BBox &box) {
    dieInfo.setDieArea(box);
}

const BBox &ktDM::getDieArea() const {
    return dieInfo.getDieArea();
}

const std::vector<std::size_t> &ktDM::getNetPins(std::size_t netId) const {
    return graph.getNetPins(netId);
}

const std::vector<std::size_t> &ktDM::getCellPins(std::size_t cellId) const {
    return graph.getCellPins(cellId);
}

const constraintMgr &ktDM::getConstraints() const {
    return fences;
}

BBox ktDM::getPlacementDieBox() const {
    return ktplace::getPlacementDieBox(dieInfo, graph);
}

void ktDM::setConstraints(constraintMgr regions) {
    fences = std::move(regions);
}

bool ktDM::hasFences() const {
    return !fences.getRegions().empty();
}

ktDM::Utilisation ktDM::measureUtilisation() const {
    Utilisation u;
    const Graph &g = getGraph();
    const std::size_t nv = g.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
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
        if (!(r.getPitch() > 0.0) || !(r.height > 0.0)) {
            continue;
        }
        u.rowHeight = std::max(u.rowHeight, r.height);
        for (const SubrowInfo &si : r.subrows) {
            if (si.getXhi(r.getPitch()) > si.getXlo()) {
                u.rowArea += (si.getXhi(r.getPitch()) - si.getXlo()) * r.height;
            }
        }
    }
    // Cells that cannot fit a single row: the other way a design can be
    // unplaceable at any density.
    if (u.rowHeight > 0.0) {
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (!vert.isFixed && !vert.isTerminal && vert.height > u.rowHeight * 1.5) {
                ++u.multiRow;
            }
        }
    }
    return u;
}

void ktDM::reportUtilisation() const {
    const Utilisation u = measureUtilisation();
    // Movable demand against the rows, which decides whether the design fits. The
    // fixed cells already occupy the rows rather than compete for them, so
    // charging their area here double-counts it (on adaptec1, 58% reads as 89%).
    const double util = u.rowArea > 0.0 ? 100.0 * u.cellArea / u.rowArea : 0.0;
    const double withFixed = u.rowArea > 0.0 ? 100.0 * (u.cellArea + u.fixedArea) / u.rowArea : 0.0;
    ktReportTable t("Design utilisation (before placement)");
    t.setHeaders({"measure", "value"});
    t.addRow({"movable cell area", std::format("{:.6e}", u.cellArea)});
    t.addRow({"fixed cell area", std::format("{:.6e}", u.fixedArea)});
    t.addRow({"row (placeable) area", std::format("{:.6e}", u.rowArea)});
    t.addRow({"utilisation (movable / rows)", std::format("{:.2}%", util)});
    t.addRow({"utilisation (incl. fixed cells)", std::format("{:.2}%", withFixed)});
    t.addRow({"movable cells", std::format("{}", u.cells)});
    if (u.rowHeight > 0.0) {
        t.addRow({"row height", std::format("{:.3}", u.rowHeight)});
        t.addRow({"cells taller than one row", std::format("{}", u.multiRow)});
    }
    t.emit();
    if (u.rowArea > 0.0 && util > 100.0) {
        ktlog.warning(
            "the design needs {:.6e} of movable cell area but only {:.6e} of row is placeable, "
            "so it is {:.1f}% full. No legal placement exists for this input: the cells do not "
            "fit, however the placer is retried.",
            u.cellArea, u.rowArea, util);
    }
    // Multi-row cells are not flagged here. This used to warn that no
    // multi-height legalizer existed and that the result would not be legal --
    // printed moments before the multi-row legalizer ran and legalized the design
    // perfectly well, so it described a limitation that had already been removed.
    // The row of the report carries the count, and legalization announces itself
    // when it picks the path.
}


void ktDM::report() const {
    const auto [numCells, numNets] = getStats();
    // What placement did the design ship with? Worth logging: if it is missing or
    // degenerate every placer silently falls back to its own seed, which looks
    // like a placer bug and is not one.
    std::size_t moved = 0;
    BBox box = BBox::getEmpty();
    const Graph &g = getGraph();
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &vert = g.getCell(v);
        box.extend(vert.x, vert.y);
        moved += (vert.x != 0.0 || vert.y != 0.0) ? 1 : 0;
    }
    ktlog.echo(
        "Loaded placement: {}/{} cells carry a position, bbox x[{:.1f},{:.1f}] y[{:.1f},{:.1f}]",
        moved, g.getNumCells(), box.lowerLeft.x, box.upperRight.x, box.lowerLeft.y,
        box.upperRight.y);
    ktlog.echo("Loaded: {} cells ({} terminals), {} nets, {} pins, {} rows", numCells,
               getNumTerminals(), numNets, getNumPins(), getNumRows());
}

std::vector<ktDM::Defect> ktDM::getDefects() const {
    std::vector<Defect> defects;
    const Graph &g = getGraph();

    // The same notion of "the die" the placer used. Two different ones would fail
    // a legal placement: the placer spreads over the union of the fixed geometry
    // and the rows, so a checker built from the
    // fixed geometry alone fails every cell in a row that reaches past the pads.
    const BBox box = getPlacementDieBox();
    std::size_t outOfDie = 0;
    std::size_t offFence = 0;
    std::size_t inForeignFence = 0;
    const std::size_t nv = g.getNumCells();
    // Row height, for deciding what counts as a tall cell below.
    double rowHeight = 0.0;
    for (const RowInfo &ri : dieInfo.getRows()) {
        if (ri.height > rowHeight) {
            rowHeight = ri.height;
        }
    }
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        const double eps = 1e-6;
        if (vert.x < box.lowerLeft.x - eps || vert.y < box.lowerLeft.y - eps ||
            vert.x + vert.width > box.upperRight.x + eps ||
            vert.y + vert.height > box.upperRight.y + eps) {
            ++outOfDie;
        }
        if (!hasFences()) {
            continue;
        }
        const BBox footprint{{vert.x, vert.y}, {vert.x + vert.width, vert.y + vert.height}};
        if (vert.regionId != constraintMgr::noRegion) {
            offFence += getConstraints().containsBox(vert.regionId, footprint) ? 0 : 1;
        } else if (!vert.isFixed && !vert.isTerminal) {
            inForeignFence += getConstraints().overlapsAnyRegion(footprint) ? 1 : 0;
        }
    }

    // Overlaps. Bulk cells go in a grid with a bin sized for a typical cell, and
    // each pair is examined once, in its bucket and the four forward neighbours.
    // The bin must NOT be sized from the largest cell: ibm01 has one cell 12752
    // units tall, so that makes the bin the size of the die and the check a
    // 12500^2 comparison. Tall cells are outside the grid for the same reason and
    // are compared against everything directly.
    std::size_t overlaps = 0;
    {
        double bin = 0.0;
        std::size_t nTall = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            // "Tall" against the rows, not an absolute height: a cell more than
            // four rows high cannot be found by a standard-cell-sized bin.
            const double tall = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (tall > 4.0) {
                ++nTall;
                continue;
            }
            bin += vert.width * vert.height;
        }
        const double meanArea = nv > 0 ? bin / std::max<std::size_t>(1, nv - nTall) : 0.0;
        double cell = meanArea > 0.0 ? std::sqrt(meanArea) : 1.0;
        const double dieW = std::max(box.upperRight.x - box.lowerLeft.x, 1.0);
        const double dieH = std::max(box.upperRight.y - box.lowerLeft.y, 1.0);
        // A few cells per bucket: enough that the map does not dominate, coarse
        // enough that a standard cell does not span many bins (which is what made
        // the original miss pairs).
        const std::size_t target = 64;
        cell = std::max(cell, std::max(dieW, dieH) / 512.0);
        cell = std::max(cell, 1e-9);
        (void)target;

        std::map<std::pair<long long, long long>, std::vector<std::size_t>> buckets;
        std::vector<std::size_t> tallCells;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            const double tall = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (tall > 4.0) {
                tallCells.push_back(v);
                continue;
            }
            const auto bx = static_cast<long long>(std::floor(vert.x / cell));
            const auto by = static_cast<long long>(std::floor(vert.y / cell));
            buckets[{bx, by}].push_back(v);
        }
        const double eps = 1e-9;
        const auto hits = [&](std::size_t a, std::size_t b) {
            const Vertex &p = g.getCell(a);
            const Vertex &q = g.getCell(b);
            // Two fixed cells overlapping is the input's business, not ours, and an I/O pin
            // is a routing-layer shape that blocks no placement site.
            if ((p.isFixed && q.isFixed) || p.isIoPin || q.isIoPin) {
                return false;
            }
            return p.x < q.x + q.width - eps && q.x < p.x + p.width - eps &&
                   p.y < q.y + q.height - eps && q.y < p.y + p.height - eps;
        };
        // Only forward neighbours: all eight examines each cross-bucket pair twice.
        static const int fwd[4][2] = {{1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (const auto &kv : buckets) {
            const std::vector<std::size_t> &mine = kv.second;
            for (std::size_t a = 0; a < mine.size(); ++a) {
                for (std::size_t b = a + 1; b < mine.size(); ++b) {
                    overlaps += hits(mine[a], mine[b]) ? 1u : 0u;
                }
            }
            for (const auto &d : fwd) {
                auto it = buckets.find({kv.first.first + d[0], kv.first.second + d[1]});
                if (it == buckets.end()) {
                    continue;
                }
                for (const std::size_t a : mine) {
                    for (const std::size_t b : it->second) {
                        overlaps += hits(a, b) ? 1u : 0u;
                    }
                }
            }
        }
        double widest = 0.0;
        double highest = 0.0;
        for (const auto &kv : buckets) {
            for (const std::size_t v : kv.second) {
                widest = std::max(widest, g.getCell(v).width);
                highest = std::max(highest, g.getCell(v).height);
            }
        }
        for (std::size_t i = 0; i < tallCells.size(); ++i) {
            const std::size_t a = tallCells[i];
            const Vertex &t = g.getCell(a);
            const auto x0 = static_cast<long long>(std::floor((t.x - widest) / cell));
            const auto x1 = static_cast<long long>(std::floor((t.x + t.width) / cell));
            const auto y0 = static_cast<long long>(std::floor((t.y - highest) / cell));
            const auto y1 = static_cast<long long>(std::floor((t.y + t.height) / cell));
            for (long long bx = x0; bx <= x1; ++bx) {
                for (auto it = buckets.lower_bound({bx, y0});
                     it != buckets.end() && it->first.first == bx && it->first.second <= y1; ++it) {
                    for (const std::size_t b : it->second) {
                        overlaps += hits(a, b) ? 1u : 0u;
                    }
                }
            }
            for (std::size_t j = i + 1; j < tallCells.size(); ++j) {
                overlaps += hits(a, tallCells[j]) ? 1u : 0u;
            }
        }
    }

    if (outOfDie > 0) {
        defects.push_back({"cells outside the die", outOfDie});
    }
    if (overlaps > 0) {
        defects.push_back({"overlapping cell pairs", overlaps});
    }
    if (offFence > 0) {
        defects.push_back({"cells outside their fence", offFence});
    }
    if (inForeignFence > 0) {
        defects.push_back({"non-member cells inside a fence", inForeignFence});
    }
    return defects;
}

void ktDM::setPlacementSolution(const solutionMgr &solution) {
    solution.commitTo(graph);
}


void ktDM::setSimplSolution(const simplSolution &solution) {
    const std::size_t n = std::min({solution.xs.size(), solution.ys.size(), graph.getNumCells()});
    for (std::size_t v = 0; v < n; ++v) {
        setCellPosition(v, solution.xs[v], solution.ys[v]);
    }
}

void ktDM::setNtuplace1Solution(const ntuplace1Solution &solution) {
    const std::size_t n = std::min({solution.xs.size(), solution.ys.size(), graph.getNumCells()});
    for (std::size_t v = 0; v < n; ++v) {
        setCellPosition(v, solution.xs[v], solution.ys[v]);
    }
}

namespace {
/// Every stage's solution is the same shape: positions indexed by cell id. One
/// writer, so a stage cannot commit a placement the others would have written
/// differently.
template <typename Solution>
void commitCells(Graph &graph, const Solution &solution) {
    const std::size_t n = std::min({solution.xs.size(), solution.ys.size(), graph.getNumCells()});
    for (std::size_t v = 0; v < n; ++v) {
        Vertex &cell = graph.getCell(v);
        cell.x = solution.xs[v];
        cell.y = solution.ys[v];
    }
}
}  // namespace

void ktDM::setAbacusSolution(const abacusSolution &solution) {
    commitCells(graph, solution);
    checkPlacement();
}

void ktDM::setMultiRowSolution(const multiRowSolution &solution) {
    commitCells(graph, solution);
    checkPlacement();
}

void ktDM::setFastdpSolution(const fastdpSolution &solution) {
    commitCells(graph, solution);
    checkPlacement();
}

solutionMgr ktDM::getPlacementSolution() const {
    return solutionMgr::fromGraph(graph);
}

double ktDM::getHpwl() const {
    // The one HPWL, in kt_solution.cc. This used to be a second implementation of
    // the same arithmetic, alongside the ones in SimplePlacer and FastDP; it is
    // now the same call they all make, over the positions as committed.
    const solutionMgr sol = getPlacementSolution();
    return getNetlistHPWL(graph, sol.getXs(), sol.getYs());
}

double ktDM::getMacroHeight() const {
    double minHeight = std::numeric_limits<double>::max();
    for (const RowInfo &ri : getRows()) {
        if (ri.height > 0.0 && !ri.subrows.empty()) {
            minHeight = std::min(minHeight, ri.height);
        }
    }
    return minHeight == std::numeric_limits<double>::max() ? minHeight : 1.5 * minHeight;
}

std::vector<std::vector<RowPiece>> ktDM::getFencedSubrows() const {
    std::vector<FenceRect> rects;
    const constraintMgr &mgr = getConstraints();
    for (int id = 0; id < static_cast<int>(mgr.getNumRegions()); ++id) {
        for (const BBox &rc : mgr.getRegion(id)->rects) {
            rects.push_back(FenceRect{rc, id});
        }
    }
    std::vector<std::vector<RowPiece>> out;
    out.reserve(getRows().size());
    for (const RowInfo &row : getRows()) {
        const double grid = row.getPitch() > 0.0 ? row.getPitch() : 1.0;
        std::vector<RowPiece> &pieces = out.emplace_back();
        for (const SubrowInfo &s : row.subrows) {
            const double xlo = s.getXlo();
            const double xhi = s.getXhi(row.getPitch());
            if (!(xhi > xlo)) {
                continue;
            }
            std::ranges::copy(splitRowByFences(xlo, xhi, row.coordinate,
                                               row.coordinate + row.height, grid, rects),
                              std::back_inserter(pieces));
        }
    }
    return out;
}

std::size_t ktDM::getMultiRowCells() const {
    const double tall = getMacroHeight();
    const Graph &g = getGraph();
    std::size_t n = 0;
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &vert = g.getCell(v);
        if (!vert.isFixed && !vert.isTerminal && vert.height > tall) {
            ++n;
        }
    }
    return n;
}

void ktDM::checkPlacement() const {
    // Timed on its own: it reads the whole placement, changes nothing, and is
    // the phase that goes quadratic if the spatial index degrades.
    const ScopedTimer checkTimer("place-check");
    const std::vector<Defect> defects = getDefects();
    ktReportTable check("Placement check (independent)");
    check.setHeaders({"check", "result"});
    if (defects.empty()) {
        check.addRow({"overlapping cell pairs", "0"});
        check.addRow({"cells outside the die", "0"});
        check.addRow({"cells outside their fence", hasFences() ? "0" : "n/a"});
        check.addRow({"non-member cells inside a fence", hasFences() ? "0" : "n/a"});
        check.addRow({"verdict", "PASS"});
    } else {
        for (const Defect &d : defects) {
            check.addRow({d.what, std::format("{}", d.count)});
        }
        check.addRow({"verdict", "FAIL"});
    }
    check.emit();
    if (!defects.empty()) {
        ktlog.warning("the placement that will be written is not legal; see the check above");
    }
}

}  // namespace ktplace

namespace ktplace {

std::uint32_t ktDM::getChecksum() const {
    std::uint32_t h = 2166136261U;
    const auto mix = [&h](const auto &value) {
        unsigned char bytes[sizeof(value)];
        std::memcpy(bytes, &value, sizeof(value));
        for (const unsigned char b : bytes) {
            h = (h ^ b) * 16777619U;
        }
    };
    const Graph &g = getGraph();
    mix(g.getNumCells());
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &cell = g.getCell(v);
        mix(cell.x);
        mix(cell.y);
        mix(cell.width);
        mix(cell.height);
        mix(static_cast<std::uint8_t>(cell.isFixed));
    }
    mix(g.getNumNets());
    for (const Pin &pin : g.getPins()) {
        mix(pin.cellId);
        mix(pin.netId);
        mix(pin.offsetX);
        mix(pin.offsetY);
    }
    return h;
}

void ktDM::reportStatistics() const {
    const Graph &g = getGraph();
    const double rowHeight = getRows().empty() ? 0.0 : getRows().front().height;
    std::size_t movable = 0, fixed = 0, macros = 0;
    double movableWidth = 0.0, macroArea = 0.0;
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &cell = g.getCell(v);
        if (cell.isFixed || cell.isTerminal) {
            if (cell.height > rowHeight) {
                ++macros;
                macroArea += cell.width * cell.height;
            }
            ++fixed;
            continue;
        }
        ++movable;
        movableWidth += cell.width;
    }

    constexpr std::array<std::size_t, 5> degreeTop{1, 2, 3, 10, 100};
    std::array<std::size_t, degreeTop.size() + 1> byDegree{};
    std::size_t maxDegree = 0;
    for (std::size_t n = 0; n < g.getNumNets(); ++n) {
        const std::size_t d = g.getNetPins(n).size();
        maxDegree = std::max(maxDegree, d);
        std::size_t bucket = 0;
        while (bucket < degreeTop.size() && d > degreeTop[bucket]) {
            ++bucket;
        }
        ++byDegree[bucket];
    }

    std::size_t sites = 0;
    double siteWidth = 0.0;
    for (const RowInfo &row : getRows()) {
        siteWidth = std::max(siteWidth, row.getPitch());
        for (const SubrowInfo &sub : row.subrows) {
            sites += static_cast<std::size_t>(sub.numSites);
        }
    }
    const BBox box = getPlacementDieBox();
    const double nets = static_cast<double>(std::max<std::size_t>(g.getNumNets(), 1));

    ktReportTable t("Design statistics");
    t.setHeaders({"measure", "value"});
    t.addRow({"placement area", std::format("({:.0f}, {:.0f}) - ({:.0f}, {:.0f})", box.lowerLeft.x,
                                            box.lowerLeft.y, box.upperRight.x, box.upperRight.y)});
    t.addRow({"rows / sites", std::format("{} / {}", getNumRows(), sites)});
    t.addRow({"site width", std::format("{:.3}", siteWidth)});
    t.addRow(
        {"cells (movable / fixed)", std::format("{} ({} / {})", g.getNumCells(), movable, fixed)});
    t.addRow({"fixed cells taller than a row (macros)", std::format("{}", macros)});
    t.addRow({"macro area", std::format("{:.6e}", macroArea)});
    t.addRow(
        {"mean movable width",
         std::format("{:.2f}", movable > 0 ? movableWidth / static_cast<double>(movable) : 0.0)});
    t.addRow({"movable cells taller than a row", std::format("{}", getMultiRowCells())});
    t.addRow({"nets / pins", std::format("{} / {}", g.getNumNets(), g.getNumPins())});
    t.addRow({"pins per net (mean / max)",
              std::format("{:.2f} / {}", static_cast<double>(g.getNumPins()) / nets, maxDegree)});
    t.addRow({"nets with 1 / 2 / 3 pins",
              std::format("{} / {} / {}", byDegree[0], byDegree[1], byDegree[2])});
    t.addRow({"nets with 4-10 / 11-100 / >100 pins",
              std::format("{} / {} / {}", byDegree[3], byDegree[4], byDegree[5])});
    t.addRow({"fence regions", std::format("{}", getConstraints().getNumRegions())});
    t.emit();
}

}  // namespace ktplace
