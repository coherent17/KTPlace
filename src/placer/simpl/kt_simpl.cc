/**
 * @file simpl.cc
 * @brief SimPL global placement. See kt_simpl.h for the algorithm summary and
 *        the bibliographic reference.
 */

#include "placer/simpl/kt_simpl.h"

#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <deque>
#include <cstdlib>
#include <fstream>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <limits>
#include <numeric>
#include <vector>

#include "util/kt_log.h"
#include "visualization/kt_plotter.h"

namespace ktplace {

namespace {

/// Sentinel for "this vertex has no variable" (it is fixed, or it is a net).
constexpr std::uint32_t kNoVar = 0xFFFFFFFFu;

/// A net's distinct pins, in ascending cell order, with each pin's offset
/// inside its cell.
struct NetInfo {
    std::vector<std::uint32_t> cell;
    std::vector<double> offX;
    std::vector<double> offY;
    double weight = 1.0;
};

/// Symmetric sparse matrix, off-diagonals in CSR plus a separate diagonal. The
/// B2B model needs no auxiliary star variables, so the variable set is exactly
/// the movable cells.
struct CsrMatrix {
    std::size_t n = 0;
    std::vector<std::size_t> rowPtr;
    std::vector<std::size_t> col;
    std::vector<double> val;
    std::vector<double> diag;

    // SpMxV is the CG inner loop and is memory-bandwidth bound, so it is the one
    // kernel that really wants threads.
    void matvec(const std::vector<double> &in, std::vector<double> &out) const {
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 256),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  double s = diag[i] * in[i];
                                  for (std::size_t e = rowPtr[i]; e < rowPtr[i + 1]; ++e) {
                                      s += val[e] * in[col[e]];
                                  }
                                  out[i] = s;
                              }
                          });
    }

    /// Sum of squares, as a parallel reduction.
    double norm2(const std::vector<double> &v) const {
        return tbb::parallel_reduce(
            tbb::blocked_range<std::size_t>(0, n, 1024), 0.0,
            [&](const tbb::blocked_range<std::size_t> &r, double acc) {
                double a = acc;
                for (std::size_t i = r.begin(); i < r.end(); ++i) {
                    a += v[i] * v[i];
                }
                return a;
            },
            [](double lhs, double rhs) {
                return lhs + rhs;
            });
    }

    /// Dot product, as a parallel reduction.
    double dot(const std::vector<double> &a, const std::vector<double> &b) const {
        return tbb::parallel_reduce(
            tbb::blocked_range<std::size_t>(0, n, 1024), 0.0,
            [&](const tbb::blocked_range<std::size_t> &r, double acc) {
                double s = acc;
                for (std::size_t i = r.begin(); i < r.end(); ++i) {
                    s += a[i] * b[i];
                }
                return s;
            },
            [](double lhs, double rhs) {
                return lhs + rhs;
            });
    }
};

/// Regular bin grid carrying the two quantities the density test needs: the cell
/// area A_c inside each bin and the available (site) area A_a.
struct DensityGrid {
    double x0 = 0.0, y0 = 0.0;
    double dx = 1.0, dy = 1.0;
    std::size_t nbx = 1, nby = 1;
    double binArea = 1.0;
    std::vector<double> occ;    // A_c
    std::vector<double> avail;  // A_a
    double totalAvail = 0.0;
    double totalCellArea = 0.0;

    std::size_t size() const {
        return nbx * nby;
    }
    std::size_t at(std::size_t ix, std::size_t iy) const {
        return iy * nbx + ix;
    }

    void locate(double px, double py, std::size_t &ix, std::size_t &iy) const {
        const double fx = std::clamp((px - x0) / dx, 0.0, static_cast<double>(nbx) - 1e-9);
        const double fy = std::clamp((py - y0) / dy, 0.0, static_cast<double>(nby) - 1e-9);
        ix = static_cast<std::size_t>(fx);
        iy = static_cast<std::size_t>(fy);
    }
    double binLoX(std::size_t ix) const {
        return x0 + static_cast<double>(ix) * dx;
    }
    double binLoY(std::size_t iy) const {
        return y0 + static_cast<double>(iy) * dy;
    }
    double hiX() const {
        return x0 + static_cast<double>(nbx) * dx;
    }
    double hiY() const {
        return y0 + static_cast<double>(nby) * dy;
    }
};

/// One pending block of the top-down partitioning (Algorithm 1, queue Q).
struct Block {
    std::size_t ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;  // inclusive bin range
    std::size_t level = 1;
    bool vertical = true;  // cut direction: vertical at level 1, then alternate
};

}  // namespace

// ---------------------------------------------------------------------------

class SimplePlacer::Impl {
public:
    explicit Impl(PlacementDB &db) : db_(db), graph_(db.getGraph()) {}

    SimplResult run(const SimplParams &P, const std::string &plotDir,
                    const std::string &snapshotDir);

private:
    // --- setup -------------------------------------------------------------
    void collect();
    void buildGrid(const SimplParams &P);
    void seedUniform(std::uint64_t seed);

    // --- net model and solver ----------------------------------------------
    void buildB2B(const std::vector<double> &px, const std::vector<double> &py, double alpha,
                  bool useAnchors);
    void solve(const std::string &tag, bool allowFrames);
    double hpwl(const std::vector<double> &px, const std::vector<double> &py) const;

    // --- density -----------------------------------------------------------
    void binCells(const std::vector<double> &px, const std::vector<double> &py);
    double densityOf(std::size_t ix0, std::size_t ix1, std::size_t iy0, std::size_t iy1) const;
    double scaledOverflow() const;

    // --- look-ahead legalization (Algorithm 1) -----------------------------
    void lookAheadLegalize();
    void processBlock(const Block &B);
    void nonlinearScale(const std::vector<std::uint32_t> &cells, std::size_t a0, std::size_t a1,
                        std::size_t b0, std::size_t b1, bool vertical, double cutCoord);

    // --- helpers -----------------------------------------------------------
    /// Centroid, per-axis spread, x-y correlation and die coverage of a position
    /// set. The correlation is the discriminator the trace needs: a population
    /// gathered onto a diagonal reads rho -> +1 with full bounding-box coverage,
    /// a uniform spread reads rho ~ 0, and a placement collapsed onto a single
    /// axis reads rho ~ 0 with a coverage near zero on one axis.
    void describe(const std::vector<double> &px, const std::vector<double> &py,
                  const char *tag) const;
    void writeFrame(const std::string &path, const std::vector<double> &px,
                    const std::vector<double> &py, double hp, double ovf, const std::string &note,
                    std::size_t step, std::size_t total);
    /// One frame from inside the CG loop: the current iterate of one axis
    /// against the other axis' last value. `tag` identifies the outer context
    /// (e.g. "init3" or "g07"), `dim` is 'x' or 'y'.
    void writeCgFrame(const std::string &tag, char dim, std::size_t cgIter, double resid);
    /// Bin-density heat map: one rectangle per bin coloured by occ/avail, so a
    /// glance shows whether the placement is spreading or still a blob.
    void writeDensityMap(const std::string &path, const std::vector<double> &px,
                         const std::vector<double> &py, const std::string &note);
    /// Bin a placement into a throwaway occupancy array, without touching the
    /// live bin index the legalizer maintains. Returns the scaled overflow.
    double binLocal(const std::vector<double> &px, const std::vector<double> &py,
                    std::vector<double> &occ) const;
    /// One trace line summarising the density field: mean/median/max bin
    /// utilisation, share of the die near capacity, empty and overfull bins.
    void densityStats(const std::vector<double> &px, const std::vector<double> &py,
                      const char *tag) const;

    PlacementDB &db_;
    Graph &graph_;
    SimplResult res_;
    SimplParams par_;

    std::size_t nv_ = 0;
    std::size_t numMovable_ = 0;

    std::vector<std::uint32_t> movVertex_;  // graph vertex id per movable slot
    std::vector<std::uint32_t> fixVertex_;
    std::vector<std::uint32_t> varOfVertex_;   // movable slot, or kNoVar
    std::vector<double> area_;                 // per movable slot
    std::vector<double> areaMovW_, areaMovH_;  // cell extents, for the macro overlap test
    std::vector<double> vx_, vy_;              // per graph vertex, live positions
    std::vector<double> pinX_, pinY_;
    std::vector<double> inputX_, inputY_;    // per movable slot, cell position
    std::vector<double> anchorX_, anchorY_;  // per movable slot, fixed pseudonet targets
    std::vector<NetInfo> nets_;              // indexed by graph vertex

    // The B2B model is separable, but the x and y graphs are NOT the same graph:
    // the extreme (min/max) pins, and therefore the edge set and every weight,
    // are chosen independently per dimension. So there are two matrices and two
    // right-hand sides. Sharing one solve vector between the axes (which is what
    // this used to do -- `lower = sol_; lowerY = sol_;`) makes x_i == y_i for
    // every cell, i.e. an exact 45-degree diagonal, which is not a placement at
    // all and is what the SVGs were showing.
    CsrMatrix Ax_, Ay_;
    std::vector<double> rhsX_, rhsY_, solX_, solY_;
    double degEps_ = 1e-9;   // below this a net is degenerate in a dimension
    double avgCellW_ = 1.0;  // mean movable cell extent, per dimension
    double avgCellH_ = 1.0;
    double rowH_ = 1.0;       // placement-row height, derived from row count
    double anchorEps_ = 1.0;  // 1.5 * row height, per ComPLx/SimPL

    DensityGrid grid_;
    double g_ = 1.0;

    /// Live index of which bin each movable cell is in, and which cells are in each
    /// bin. processBlock() needs the cells inside a block, and it used to find them
    /// by scanning every movable cell. With 57k-90k blocks in the later legalizer
    /// rounds and ~210k cells that is ~1e10 tests per round, which dominated
    /// everything. The index is kept exact as cells move: nonlinearScale() relocates
    /// a cell between bins in O(1) via swap-and-pop.
    std::vector<std::vector<std::uint32_t>> binCells_;
    std::vector<std::uint32_t> cellBin_;
    std::vector<std::uint32_t> cellSlot_;

    /// Move one cell's bin membership, keeping both the index AND the density
    /// field exact.
    ///
    /// The occupancy matters as much as the index. The legalizer relocates cells
    /// as it recurses, but the density field was previously computed once per
    /// lookAheadLegalize() call and never refreshed, so every C_c cell-area
    /// median, every C_B whitespace median and every region-density test was
    /// derived from occupancy describing where the cells were BEFORE the
    /// redistribution, while the cells being redistributed had already moved.
    /// That is the most likely cause of sub-regions being handed more area than
    /// they can hold, of cells being scaled into each other, and therefore of the
    /// negative lower/upper gap that means the "legalized" cells overlap.
    void rehome(std::uint32_t cell, std::size_t toBin) {
        const std::uint32_t from = cellBin_[cell];
        if (from == toBin) {
            return;
        }
        grid_.occ[from] -= area_[cell];
        grid_.occ[toBin] += area_[cell];
        std::vector<std::uint32_t> &src = binCells_[from];
        const std::size_t slot = cellSlot_[cell];
        const std::uint32_t last = src.back();
        src[slot] = last;
        cellSlot_[last] = static_cast<std::uint32_t>(slot);
        src.pop_back();
        binCells_[toBin].push_back(cell);
        cellSlot_[cell] = static_cast<std::uint32_t>(binCells_[toBin].size() - 1);
        cellBin_[cell] = static_cast<std::uint32_t>(toBin);
    }

    BBox die_{};
    double dieW_ = 1.0, dieH_ = 1.0;

    /// Queue Q of Algorithm 1.
    std::deque<Block> pending_;
    std::size_t blocksProcessed_ = 0;
    std::size_t deepestLevel_ = 0;
    std::size_t maxBlockCells_ = 0;

    /// Where placement frames go (<plotDir>/simpl, or the snapshot directory
    /// when no plot directory was given). Empty disables all frames.
    std::string frameDir_;
    /// Outer-loop context for the CG frames of the solve currently running.
    std::string cgTag_;
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::collect() {
    nv_ = graph_.getNumVertices();

    varOfVertex_.assign(nv_, kNoVar);
    std::vector<NetInfo> nets(nv_);
    std::size_t netCount = 0;

    for (std::size_t v = 0; v < nv_; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type == VertexType::Net) {
            ++netCount;
            NetInfo &ni = nets[v];
            ni.weight = vert.weight;
            ni.cell.reserve(vert.inEdges.size());
            for (const std::size_t eid : vert.inEdges) {
                const Edge &e = graph_.getEdge(eid);
                ni.cell.push_back(e.source);
                ni.offX.push_back(e.offsetX);
                ni.offY.push_back(e.offsetY);
            }
            // Sort by cell, then drop repeated pins on one cell, keeping the
            // offsets aligned. A cell listed twice on a net must contribute
            // once, or the B2B degree k and the clique expansion are both wrong.
            std::vector<std::uint32_t> order(ni.cell.size());
            std::iota(order.begin(), order.end(), 0u);
            std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
                return ni.cell[a] < ni.cell[b];
            });
            NetInfo dedup;
            dedup.weight = ni.weight;
            dedup.cell.reserve(order.size());
            for (const std::uint32_t oi : order) {
                if (!dedup.cell.empty() && dedup.cell.back() == ni.cell[oi]) {
                    continue;
                }
                dedup.cell.push_back(ni.cell[oi]);
                dedup.offX.push_back(ni.offX[oi]);
                dedup.offY.push_back(ni.offY[oi]);
            }
            nets[v] = std::move(dedup);
            continue;
        }
        if (vert.isFixed || vert.isTerminal) {
            fixVertex_.push_back(static_cast<std::uint32_t>(v));
            continue;
        }
        varOfVertex_[v] = static_cast<std::uint32_t>(numMovable_++);
        movVertex_.push_back(static_cast<std::uint32_t>(v));
        // Keep whatever placement the design shipped with. It used to be dropped
        // on the floor here and overwritten by a uniform seed, so a Bookshelf .pl
        // carrying a good solution was never even looked at.
        inputX_.push_back(vert.x);
        inputY_.push_back(vert.y);
        area_.push_back(vert.width * vert.height);
        areaMovW_.push_back(vert.width);
        areaMovH_.push_back(vert.height);
    }
    res_.numMovable = numMovable_;
    res_.numFixed = fixVertex_.size();
    res_.nets = netCount;
    nets_ = std::move(nets);

    // Net-degree shape and pin totals. A design that is nearly all 2-pin nets
    // never exercises the B2B extreme-to-all expansion, so a B2B bug would hide
    // there; adaptec1 is 52% 2-pin and 46% higher degree, which does exercise it.
    // nets_ is indexed by VERTEX id, so it must be walked with the graph's
    // vertex types: the cell entries are empty and would otherwise be counted as
    // one-pin nets.
    {
        std::size_t twoPin = 0, multiPin = 0, singlePin = 0, pins = 0, maxDeg = 0;
        double weightSum = 0.0, weightMin = std::numeric_limits<double>::max();
        double weightMax = -std::numeric_limits<double>::max();
        for (std::size_t v = 0; v < nv_; ++v) {
            if (graph_.getVertex(v).type != VertexType::Net) {
                continue;
            }
            const NetInfo &ni = nets_[v];
            const std::size_t k = ni.cell.size();
            pins += k;
            maxDeg = std::max(maxDeg, k);
            weightSum += ni.weight;
            weightMin = std::min(weightMin, ni.weight);
            weightMax = std::max(weightMax, ni.weight);
            if (k < 2) {
                ++singlePin;
            } else if (k == 2) {
                ++twoPin;
            } else {
                ++multiPin;
            }
        }
        const double invN = 1.0 / static_cast<double>(std::max(netCount, std::size_t{1}));
        ktlog.trace(
            "nets: {} total, {} single-pin, {} two-pin ({:.1f}%), {} multi-pin ({:.1f}%), "
            "max degree {}, {:.1f} pins/net; weight mean {:.4g} range [{:.4g},{:.4g}]",
            netCount, singlePin, twoPin, 100.0 * static_cast<double>(twoPin) * invN, multiPin,
            100.0 * static_cast<double>(multiPin) * invN, maxDeg, static_cast<double>(pins) * invN,
            weightSum * invN, weightMin, weightMax);
    }

    Ax_.n = numMovable_;
    Ay_.n = numMovable_;
    solX_.assign(numMovable_, 0.0);
    solY_.assign(numMovable_, 0.0);
    rhsX_.assign(numMovable_, 0.0);
    rhsY_.assign(numMovable_, 0.0);
    pinX_.assign(numMovable_, 0.0);
    pinY_.assign(numMovable_, 0.0);
    if (inputX_.size() != numMovable_) {
        inputX_.assign(numMovable_, 0.0);
        inputY_.assign(numMovable_, 0.0);
    }
    anchorX_.assign(numMovable_, 0.0);
    anchorY_.assign(numMovable_, 0.0);
    vx_.assign(nv_, 0.0);
    vy_.assign(nv_, 0.0);
    for (std::size_t v = 0; v < nv_; ++v) {
        vx_[v] = graph_.getVertex(v).x;
        vy_[v] = graph_.getVertex(v).y;
    }
}

void SimplePlacer::Impl::buildGrid(const SimplParams &P) {
    // Die extent: the parsed die area when it plausibly contains the fixed
    // cells, otherwise the fixed-cell bounding box.
    BBox die = fixedCellBBox(graph_);
    const auto da = db_.getDieArea();
    if (da.second.first > da.first.first && da.second.second > da.first.second) {
        bool contains = true;
        for (const std::uint32_t v : fixVertex_) {
            const Vertex &vert = graph_.getVertex(v);
            if (vert.x < da.first.first - 1.0 || vert.y < da.first.second - 1.0 ||
                vert.x + vert.width > da.second.first + 1.0 ||
                vert.y + vert.height > da.second.second + 1.0) {
                contains = false;
                break;
            }
        }
        if (contains) {
            die = {da.first.first, da.first.second, da.second.first, da.second.second};
        }
    }
    // Fall back to the rows. A Bookshelf design need not have any fixed cell
    // and need not declare a die area, and without this such a design gets a
    // 1x1 die: dma reported "available area 1" and a utilisation of 1.6e13%,
    // which poisoned the density grid and made its input placement look
    // degenerate. The rows are the authoritative statement of where cells may
    // go, so they are a better source than an empty fixed-cell bounding box.
    if (!(die[2] - die[0] > 1.0) || !(die[3] - die[1] > 1.0)) {
        const std::vector<PlacementDB::RowInfo> rows = db_.getRows();
        double rlo = 0.0, rhi = 0.0, blo = 0.0, bhi = 0.0;
        bool any = false;
        for (const PlacementDB::RowInfo &ri : rows) {
            if (!(ri.pitch() > 0.0)) {
                continue;
            }
            if (!any) {
                rlo = ri.coordinate;
                rhi = ri.coordinate + ri.height;
                blo = ri.xlo();
                bhi = ri.xhi();
                any = true;
                continue;
            }
            rlo = std::min(rlo, ri.coordinate);
            rhi = std::max(rhi, ri.coordinate + ri.height);
            blo = std::min(blo, ri.xlo());
            bhi = std::max(bhi, ri.xhi());
        }
        if (any && (bhi > blo) && (rhi > rlo)) {
            die = BBox{blo, rlo, bhi, rhi};
        }
    }
    die_ = die;
    dieW_ = std::max(die[2] - die[0], 1e-9);
    dieH_ = std::max(die[3] - die[1], 1e-9);
    degEps_ = 1e-9 * std::min(dieW_, dieH_);

    // Mean movable cell extent, and the row height. The datamodel exposes only a
    // row COUNT, so the height is derived geometrically, which is exact for the
    // uniform-row Bookshelf suites and an approximation otherwise.
    double sumW = 0.0;
    double sumH = 0.0;
    std::size_t cnt = 0;
    for (const std::uint32_t v : movVertex_) {
        const Vertex &vert = graph_.getVertex(v);
        sumW += vert.width;
        sumH += vert.height;
        ++cnt;
    }
    if (cnt > 0) {
        avgCellW_ = sumW / static_cast<double>(cnt);
        avgCellH_ = sumH / static_cast<double>(cnt);
    }
    const std::size_t nRows = std::max<std::size_t>(db_.getNumRows(), 1);
    rowH_ = dieH_ / static_cast<double>(nRows);
    // ComPLx (Kim & Markov, DAC 2012) states this for SimPL/SimPLR verbatim:
    // "each movable object is connected to its anchor location by a pseudonet,
    //  contributing w_i (x_i - x_i^0)^2 ... where w_i = lambda/(|x_i - x_i^0| +
    //  eps). eps > 0 is used to bound the denominator away from zero and make the
    //  objective function strictly convex. In SimPL and SimPLR, eps is
    //  calculated as 1.5 times row height."
    anchorEps_ = 1.5 * rowH_;
    ktlog.trace(
        "mean cell {:.4g} x {:.4g}, row height {:.4g}, anchor eps {:.4g} "
        "(= 1.5 rows)",
        avgCellW_, avgCellH_, rowH_, anchorEps_);

    std::size_t nbx = P.binsX;
    std::size_t nby = P.binsY;
    if (nbx == 0 || nby == 0) {
        // A few cells per bin keeps the BFS clustering and the cutline search
        // cheap while still resolving the density of a large design.
        const double t =
            std::clamp(std::sqrt(static_cast<double>(std::max<std::size_t>(numMovable_, 1)) / 4.0),
                       16.0, 256.0);
        nbx = static_cast<std::size_t>(t);
        nby = static_cast<std::size_t>(t);
    }
    grid_.x0 = die[0];
    grid_.y0 = die[1];
    grid_.nbx = nbx;
    grid_.nby = nby;
    grid_.dx = dieW_ / static_cast<double>(nbx);
    grid_.dy = dieH_ / static_cast<double>(nby);
    grid_.binArea = grid_.dx * grid_.dy;
    grid_.occ.assign(nbx * nby, 0.0);
    grid_.avail.assign(nbx * nby, grid_.binArea);
    res_.binsX = nbx;
    res_.binsY = nby;

    // Available area per bin: the bin area less whatever a fixed macro covers.
    // (The paper's A_a is the cell-site area; with row-aligned bins and g = 1
    // the bin area is the site area exactly, so the only thing to remove is
    // blockage.)
    for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
        grid_.avail[k] = grid_.binArea;
    }
    for (const std::uint32_t v : fixVertex_) {
        const Vertex &vert = graph_.getVertex(v);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        std::size_t ix0, iy0, ix1, iy1;
        grid_.locate(vx_[v], vy_[v], ix0, iy0);
        grid_.locate(vx_[v] + vert.width, vy_[v] + vert.height, ix1, iy1);
        for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
            for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                const double bx = grid_.binLoX(ix);
                const double by = grid_.binLoY(iy);
                const double ox = std::max(
                    0.0, std::min(bx + grid_.dx, vx_[v] + vert.width) - std::max(bx, vx_[v]));
                const double oy = std::max(
                    0.0, std::min(by + grid_.dy, vy_[v] + vert.height) - std::max(by, vy_[v]));
                grid_.avail[grid_.at(ix, iy)] =
                    std::max(grid_.avail[grid_.at(ix, iy)] - ox * oy, 0.0);
            }
        }
    }
    // Smallest row site pitch, used as the sliver threshold below.
    double ri_pitch_floor = degEps_;
    for (const PlacementDB::RowInfo &r : db_.getRows()) {
        if (r.pitch() > 0.0) {
            ri_pitch_floor = std::max(ri_pitch_floor, r.pitch() * r.height);
        }
    }
    // A_a is the available *cell-site* area of a bin, not its geometric area
    // (Section 4.2). Using binArea overstates it twice over: bins that fall
    // outside the row band have no sites at all yet were counted as fully
    // available, and a bin that a macro almost fills was left with a tiny
    // positive remainder. Those slivers then read as density ~1e15 in the
    // overfill test, and densityOf() inside the region-growth loop saw an
    // astronomic density for any rectangle containing one, so it expanded to the
    // die boundary every time and the legalizer scattered cells over the whole
    // chip. Measured: 169 such bins on adaptec1.
    //
    // So rebuild avail from the rows: the site area of a bin is the y-overlap
    // with each row times the x-extent of that row's subrows inside the bin,
    // summed over rows, less macro coverage.
    {
        const std::vector<PlacementDB::RowInfo> rowInfo = db_.getRows();
        std::fill(grid_.avail.begin(), grid_.avail.end(), 0.0);
        for (const PlacementDB::RowInfo &ri : rowInfo) {
            if (!(ri.pitch() > 0.0) || !(ri.height > 0.0)) {
                continue;
            }
            const double ry1 = ri.coordinate + ri.height;
            std::size_t iy0, iy1, dummy;
            grid_.locate(ri.coordinate, ry1, iy0, iy1);
            (void)dummy;
            for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
                const double bLo = grid_.binLoY(iy);
                const double bHi = bLo + grid_.dy;
                const double yOv = std::max(0.0, std::min(bHi, ry1) - std::max(bLo, ri.coordinate));
                if (!(yOv > 0.0)) {
                    continue;
                }
                for (const PlacementDB::SubrowInfo &si : ri.subrows) {
                    if (!(si.xhi(ri.pitch()) > si.xlo())) {
                        continue;
                    }
                    std::size_t ix0, ix1;
                    grid_.locate(si.xlo(), si.xhi(ri.pitch()), ix0, ix1);
                    for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                        const double xLo = grid_.binLoX(ix);
                        const double xHi = xLo + grid_.dx;
                        const double xOv = std::max(
                            0.0, std::min(xHi, si.xhi(ri.pitch())) - std::max(xLo, si.xlo()));
                        if (!(xOv > 0.0)) {
                            continue;
                        }
                        grid_.avail[grid_.at(ix, iy)] += xOv * yOv;
                    }
                }
            }
        }
        // Subtract macro coverage, then clamp: a bin with no room left must be
        // exactly zero, never a sliver.
        for (const std::uint32_t fv : fixVertex_) {
            const Vertex &vert = graph_.getVertex(fv);
            if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
                continue;
            }
            std::size_t ix0, iy0, ix1, iy1;
            grid_.locate(vx_[fv], vy_[fv], ix0, iy0);
            grid_.locate(vx_[fv] + vert.width, vy_[fv] + vert.height, ix1, iy1);
            for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
                for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                    const double bx = grid_.binLoX(ix);
                    const double by = grid_.binLoY(iy);
                    const double ox = std::max(
                        0.0, std::min(bx + grid_.dx, vx_[fv] + vert.width) - std::max(bx, vx_[fv]));
                    const double oy = std::max(0.0, std::min(by + grid_.dy, vy_[fv] + vert.height) -
                                                        std::max(by, vy_[fv]));
                    grid_.avail[grid_.at(ix, iy)] =
                        std::max(grid_.avail[grid_.at(ix, iy)] - ox * oy, 0.0);
                }
            }
        }
        // A sliver below a thousandth of a site cannot hold anything; treating it
        // as unavailable keeps the density ratios bounded.
        const double sliver = 1e-3 * ri_pitch_floor;
        for (double &a : grid_.avail) {
            if (a < sliver) {
                a = (a > 0.0) ? 0.0 : a;
            }
        }
    }
    grid_.totalAvail = 0.0;
    for (const double a : grid_.avail) {
        grid_.totalAvail += a;
    }
    grid_.totalCellArea = std::accumulate(area_.begin(), area_.end(), 0.0);
}

void SimplePlacer::Impl::seedUniform(std::uint64_t seed) {
    // The paper seeds with a uniformly distributed placement, not a single
    // collapsed point. That matters for B2B in particular: the model needs
    // distinct extreme pins, and a single point would make every net degenerate
    // in both dimensions on the first build.
    std::uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53);
    };
    for (std::size_t i = 0; i < numMovable_; ++i) {
        pinX_[i] = die_[0] + next() * dieW_;
        pinY_[i] = die_[1] + next() * dieH_;
    }
}

// ---------------------------------------------------------------------------
// B2B net model and the linear solve
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::buildB2B(const std::vector<double> &px, const std::vector<double> &py,
                                  double alpha, bool useAnchors) {
    // Pin coordinates per cell, fixed ones included: a B2B edge to a fixed cell is
    // built exactly like one to a movable cell and then eliminated into the
    // diagonal and the right-hand side.
    std::vector<double> cx(nv_), cy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        cx[v] = vx_[v];
        cy[v] = vy_[v];
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        cx[movVertex_[i]] = px[i];
        cy[movVertex_[i]] = py[i];
    }

    // Minimum edge length for the 1/length weight. Chu, "Electronic Design
    // Automation" ch. 11 sec. 11.5.2.2, gives the reason almost verbatim: "Nets
    // becoming very short (i.e. g_ie becoming very small) may cause numerical
    // problems during the minimization of L_star. Therefore, g_ie is lower bounded
    // (by the average module width for example) to ensure that g_ie will never be
    // zero." The same defect sits in the denominator of the BoundingBox edge
    // weight. Without the floor, a few near-coincident nets reached lengths of
    // ~1e-5, took weights of ~1e5 each, lifted the interconnect diagonal to
    // 2.8e8, and cut the pseudonet share to 2e-9.
    const double lenFloor[2] = {std::max(avgCellW_, degEps_), std::max(avgCellH_, degEps_)};

    struct Triple {
        std::size_t col;
        double val;
    };

    double anchorDiagSum = 0.0;
    std::size_t nnzX = 0, nnzY = 0;
    std::vector<double> pinCoord;

    for (int dim = 0; dim < 2; ++dim) {
        CsrMatrix &A = (dim == 0) ? Ax_ : Ay_;
        std::vector<double> &rhs = (dim == 0) ? rhsX_ : rhsY_;
        const std::vector<double> &coord = (dim == 0) ? cx : cy;

        std::vector<std::vector<Triple>> rows(numMovable_);
        std::vector<double> diag(numMovable_, 0.0);
        std::fill(rhs.begin(), rhs.end(), 0.0);

        // One B2B edge: symmetric between two movable cells, or eliminated into
        // the diagonal and the RHS when one end is fixed.
        const auto addEdge = [&](std::size_t cellA, double posA, std::size_t cellB, double posB,
                                 double w) {
            const std::uint32_t va = varOfVertex_[cellA];
            const std::uint32_t vb = varOfVertex_[cellB];
            if (va != kNoVar && vb != kNoVar) {
                rows[va].push_back({vb, -w});
                rows[vb].push_back({va, -w});
                diag[va] += w;
                diag[vb] += w;
            } else if (va != kNoVar) {
                diag[va] += w;
                rhs[va] += w * posB;
            } else if (vb != kNoVar) {
                diag[vb] += w;
                rhs[vb] += w * posA;
            }
            // both fixed: a constant, no derivative.
        };

        for (std::size_t v = 0; v < nv_; ++v) {
            const NetInfo &ni = nets_[v];
            const std::size_t k = ni.cell.size();
            if (k < 2) {
                continue;
            }
            const double base = ni.weight / static_cast<double>(k - 1);

            pinCoord.resize(k);
            const std::vector<double> &off = (dim == 0) ? ni.offX : ni.offY;
            for (std::size_t q = 0; q < k; ++q) {
                pinCoord[q] = coord[ni.cell[q]] + off[q];
            }
            std::size_t lo = 0;
            std::size_t hi = 0;
            for (std::size_t q = 1; q < k; ++q) {
                if (pinCoord[q] < pinCoord[lo]) {
                    lo = q;
                }
                if (pinCoord[q] > pinCoord[hi]) {
                    hi = q;
                }
            }
            const double span = pinCoord[hi] - pinCoord[lo];
            if (!(span > degEps_)) {
                // Degenerate in this dimension: the bounding-box length here is
                // zero, so the net contributes nothing to this solve.
                continue;
            }
            // Extremes to each other ...
            addEdge(ni.cell[lo], pinCoord[lo], ni.cell[hi], pinCoord[hi],
                    base / std::max(span, lenFloor[dim]));
            // ... and each extreme to every other pin, with the length floored so
            // a pin sitting on an extreme cannot produce an unbounded weight.
            for (std::size_t q = 0; q < k; ++q) {
                if (q == lo || q == hi) {
                    continue;
                }
                addEdge(ni.cell[lo], pinCoord[lo], ni.cell[q], pinCoord[q],
                        base / std::max(std::abs(pinCoord[q] - pinCoord[lo]), lenFloor[dim]));
                addEdge(ni.cell[hi], pinCoord[hi], ni.cell[q], pinCoord[q],
                        base / std::max(std::abs(pinCoord[q] - pinCoord[hi]), lenFloor[dim]));
            }
        }

        // Pseudonets: each cell is wired to its fixed, zero-area anchor with a
        // spring of stiffness alpha. The anchor is fixed, so this only adds to the
        // diagonal -- which is exactly why the paper can claim it improves diagonal
        // dominance and speeds up the Jacobi-preconditioned CG.
        //
        // Weight = alpha, i.e. a constant-stiffness spring, NOT alpha/length.
        // Figure 6 labels the pseudonet "weight = alpha/Length", but reading
        // Length as the raw cell-to-anchor distance makes the scheme unusable, and
        // the arithmetic is unambiguous. Measured on adaptec1 (see the per-cell
        // "pseudonet share" trace): the per-cell interconnect diagonal is ~0.16,
        // while a lower bound sits ~1000 units from its legal anchor, so
        //     w = alpha/(d + eps)  ->  anchor share ~1e-4, never moves a cell
        //     w = alpha            ->  anchor share reaches parity around
        //                             iteration 15, inside the paper's 26-35
        //                             global-placement iterations.
        // So the figure's Length must be a normalised length of order 1, in which
        // case alpha/Length reduces to alpha up to a constant the published
        // schedule is already calibrated against.
        if (useAnchors) {
            for (std::size_t i = 0; i < numMovable_; ++i) {
                const double anchor = (dim == 0) ? anchorX_[i] : anchorY_[i];
                double w = alpha;
                if (par_.pseudonetLaw == SimplParams::PseudonetLaw::InverseLength) {
                    // alpha / distance, with the same length floor as a B2B edge.
                    // The floor matters most exactly where the paper's initial
                    // placement puts everything: all cells start near the centre,
                    // so distance is often ~0 and the weight is otherwise
                    // unbounded.
                    const double d = std::max(std::abs(px[i] - anchor), lenFloor[dim]);
                    w = alpha / d;
                }
                diag[i] += w;
                rhs[i] += w * anchor;
                if (dim == 0) {
                    anchorDiagSum += w;
                }
            }
        }

        // Assemble CSR. Duplicate (row,col) pairs are summed by matvec, which is
        // the correct superposition of weights.
        A.rowPtr.assign(numMovable_ + 1, 0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            A.rowPtr[i + 1] = A.rowPtr[i] + rows[i].size();
        }
        A.col.assign(A.rowPtr[numMovable_], 0);
        A.val.assign(A.rowPtr[numMovable_], 0.0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            std::size_t e = A.rowPtr[i];
            for (const Triple &t : rows[i]) {
                A.col[e] = t.col;
                A.val[e] = t.val;
                ++e;
            }
        }
        // A cell with no nets would otherwise have a zero diagonal, and the Jacobi
        // preconditioner 1/diag would be unbounded.
        A.diag.assign(numMovable_, 0.0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            A.diag[i] = std::max(diag[i], 1e-12);
        }
        if (dim == 0) {
            nnzX = A.rowPtr[numMovable_];
        } else {
            nnzY = A.rowPtr[numMovable_];
        }
    }

    // How much of the system the pseudonets actually control.
    double wlDiag = 0.0;
    for (std::size_t i = 0; i < numMovable_; ++i) {
        wlDiag += Ax_.diag[i];
        if (useAnchors && par_.pseudonetLaw == SimplParams::PseudonetLaw::ConstantStiffness) {
            wlDiag -= alpha;
        }
    }
    ktlog.trace("  b2b: {} x-edges, {} y-edges ({:.2f}/{:.2f} per cell)", nnzX, nnzY,
                static_cast<double>(nnzX) / std::max<std::size_t>(numMovable_, 1),
                static_cast<double>(nnzY) / std::max<std::size_t>(numMovable_, 1));
    if (useAnchors) {
        const double perCell = 1.0 / std::max<std::size_t>(numMovable_, 1);
        ktlog.trace(
            "  pseudonet share: anchor diagonal {:.4g} vs interconnect diagonal {:.4g} "
            "= {:.3g} (alpha {:.4g}); per cell {:.4g} vs {:.4g}",
            anchorDiagSum, wlDiag, anchorDiagSum / std::max(wlDiag, 1e-300), alpha,
            anchorDiagSum * perCell, wlDiag * perCell);
    }
}

void SimplePlacer::Impl::solve(const std::string &tag, bool allowFrames) {
    // Jacobi-preconditioned CG, run once per axis. The x and y systems are
    // different matrices with different right-hand sides, so they are solved
    // separately; the B2B model is separable, which is why this is two clean
    // SPD systems rather than one coupled 2n-by-2n one.
    //
    // `tag` identifies the outer context ("init3", "g07", ...) for the
    // per-CG-iteration frames. A frame shows the axis currently being solved
    // against the other axis' current value -- the honest view of CG
    // convergence. Frames are disabled for the initial-placement solves: that
    // phase is a monotone descent with nothing to diagnose, and emitting frames
    // for it would bury the global loop under thousands of files.
    cgTag_ = tag;
    const auto one = [&](const CsrMatrix &A, const std::vector<double> &b, std::vector<double> &x,
                         char dim) {
        const std::size_t n = A.n;
        std::vector<double> r(n), z(n), p(n), Ap(n);
        std::vector<double> invDiag(n);
        for (std::size_t i = 0; i < n; ++i) {
            invDiag[i] = 1.0 / A.diag[i];
        }
        A.matvec(x, Ap);
        double bNorm2 = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            r[i] = b[i] - Ap[i];
            bNorm2 += b[i] * b[i];
        }
        const double bNorm = std::sqrt(bNorm2);
        if (!(bNorm > 0.0)) {
            return std::make_tuple(0u, 0u, 0.0);
        }
        double rho = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            z[i] = invDiag[i] * r[i];
            rho += r[i] * z[i];
        }
        p = z;
        double rs = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            rs += r[i] * r[i];
        }
        double resid = std::sqrt(rs) / bNorm;
        std::size_t iters = 0;
        std::size_t itersToTol = 0;
        for (std::size_t it = 0; it < par_.cgMaxIter && resid > par_.cgTol; ++it) {
            iters = it + 1;
            A.matvec(p, Ap);
            double pAp = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                pAp += p[i] * Ap[i];
            }
            if (!(pAp > 0.0)) {
                break;
            }
            const double alpha = rho / pAp;
            rs = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                x[i] += alpha * p[i];
                r[i] -= alpha * Ap[i];
                rs += r[i] * r[i];
            }
            resid = std::sqrt(rs) / bNorm;
            if (itersToTol == 0 && resid <= 1e-3) {
                itersToTol = iters;
            }
            if (allowFrames && par_.cgEvery > 0 && !frameDir_.empty() &&
                (iters % par_.cgEvery) == 0) {
                writeCgFrame(tag, dim, iters, resid);
            }
            double rhoNew = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                z[i] = invDiag[i] * r[i];
                rhoNew += r[i] * z[i];
            }
            if (!(rhoNew > 0.0)) {
                break;
            }
            const double beta = rhoNew / rho;
            for (std::size_t i = 0; i < n; ++i) {
                p[i] = z[i] + beta * p[i];
            }
            rho = rhoNew;
        }
        return std::make_tuple(static_cast<unsigned>(iters), static_cast<unsigned>(itersToTol),
                               resid);
    };

    const auto rx = one(Ax_, rhsX_, solX_, 'x');
    const auto ry = one(Ay_, rhsY_, solY_, 'y');
    ktlog.trace(
        "  cg: x {} iteration(s) (1e-3 at {}) residual {:.3e}, y {} iteration(s) (1e-3 at {}) "
        "residual {:.3e}",
        std::get<0>(rx), std::get<1>(rx), std::get<2>(rx), std::get<0>(ry), std::get<1>(ry),
        std::get<2>(ry));
}

double SimplePlacer::Impl::hpwl(const std::vector<double> &px,
                                const std::vector<double> &py) const {
    std::vector<double> cx(nv_), cy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        cx[v] = vx_[v];
        cy[v] = vy_[v];
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        cx[movVertex_[i]] = px[i];
        cy[movVertex_[i]] = py[i];
    }
    double total = 0.0;
    for (std::size_t v = 0; v < nv_; ++v) {
        const NetInfo &ni = nets_[v];
        if (ni.cell.size() < 2) {
            continue;
        }
        double x0 = std::numeric_limits<double>::max();
        double x1 = -std::numeric_limits<double>::max();
        double y0 = std::numeric_limits<double>::max();
        double y1 = -std::numeric_limits<double>::max();
        for (std::size_t q = 0; q < ni.cell.size(); ++q) {
            const double x = cx[ni.cell[q]] + ni.offX[q];
            const double y = cy[ni.cell[q]] + ni.offY[q];
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        total += (x1 - x0) + (y1 - y0);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Density
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::binCells(const std::vector<double> &px, const std::vector<double> &py) {
    std::fill(grid_.occ.begin(), grid_.occ.end(), 0.0);
    for (auto &b : binCells_) {
        b.clear();
    }
    if (cellBin_.size() != numMovable_) {
        binCells_.assign(grid_.size(), {});
        cellBin_.assign(numMovable_, 0);
        cellSlot_.assign(numMovable_, 0);
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        std::size_t ix, iy;
        grid_.locate(px[i], py[i], ix, iy);
        const std::size_t k = grid_.at(ix, iy);
        grid_.occ[k] += area_[i];
        cellBin_[i] = static_cast<std::uint32_t>(k);
        cellSlot_[i] = static_cast<std::uint32_t>(binCells_[k].size());
        binCells_[k].push_back(static_cast<std::uint32_t>(i));
    }
}

double SimplePlacer::Impl::densityOf(std::size_t ix0, std::size_t ix1, std::size_t iy0,
                                     std::size_t iy1) const {
    double c = 0.0;
    double a = 0.0;
    for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
        for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            c += grid_.occ[k];
            a += grid_.avail[k];
        }
    }
    return (a > 0.0) ? (c / a) : 0.0;
}

double SimplePlacer::Impl::scaledOverflow() const {
    if (!(grid_.totalAvail > 0.0)) {
        return 0.0;
    }
    double ex = 0.0;
    for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
        const double cap = g_ * grid_.avail[k];
        if (grid_.occ[k] > cap) {
            ex += grid_.occ[k] - cap;
        }
    }
    return ex / grid_.totalAvail;
}

// ---------------------------------------------------------------------------
// Look-ahead legalization (Algorithm 1)
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::nonlinearScale(const std::vector<std::uint32_t> &cells, std::size_t a0,
                                        std::size_t a1, std::size_t b0, std::size_t b1,
                                        bool vertical, double cutCoord) {
    if (cells.empty() || a0 > a1 || b0 > b1) {
        return;
    }
    const DensityGrid &g = grid_;

    // (i) Stripe boundaries. Cutlines are drawn along obstacle edges first, then
    // any stripe still holding more than 1/10 of the region's available area is
    // subdivided, so no single stripe is a large target that would leave the
    // others empty.
    std::vector<std::size_t> bounds;
    bounds.push_back(a0);
    bounds.push_back(a1 + 1);
    for (const std::uint32_t fv : fixVertex_) {
        const Vertex &vert = graph_.getVertex(fv);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        const double lo = vertical ? vx_[fv] : vy_[fv];
        const double len = vertical ? vert.width : vert.height;
        const double axis0 = vertical ? g.x0 : g.y0;
        const double dAxis = vertical ? g.dx : g.dy;
        const double nBins = static_cast<double>(vertical ? g.nbx : g.nby);
        // The obstacle's two edges, as bin indices along the cut axis.
        const std::size_t t0 =
            static_cast<std::size_t>(std::clamp((lo - axis0) / dAxis, 0.0, nBins - 1e-9));
        const std::size_t t1 =
            static_cast<std::size_t>(std::clamp((lo + len - axis0) / dAxis, 0.0, nBins - 1e-9));
        if (t1 < a0 || t0 > a1) {
            continue;  // obstacle outside this block
        }
        const std::size_t c0 = std::max(t0, a0);
        const std::size_t c1 = std::min(t1, a1);
        bounds.push_back(c0);
        bounds.push_back(c1 + 1);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    // Available area of the whole block, in the cut direction.
    double regionAvail = 0.0;
    for (std::size_t t = a0; t <= a1; ++t) {
        for (std::size_t s = b0; s <= b1; ++s) {
            regionAvail += g.avail[vertical ? g.at(t, s) : g.at(s, t)];
        }
    }
    const double frac = par_.stripeAreaFraction * regionAvail;

    // Subdivide over-large stripes until every one is small enough.
    for (std::size_t bi = 0; bi + 1 < bounds.size();) {
        const std::size_t s0 = bounds[bi];
        const std::size_t s1 = bounds[bi + 1] - 1;
        double sa = 0.0;
        for (std::size_t t = s0; t <= s1; ++t) {
            for (std::size_t s = b0; s <= b1; ++s) {
                sa += g.avail[vertical ? g.at(t, s) : g.at(s, t)];
            }
        }
        if (sa > frac && s1 > s0) {
            const std::size_t mid = (s0 + s1) / 2;
            bounds.insert(bounds.begin() + static_cast<long>(bi) + 1, mid + 1);
            continue;  // re-test the two halves
        }
        ++bi;
    }

    const std::size_t nStripes = bounds.size() - 1;
    if (nStripes == 0) {
        return;
    }

    // Stripe geometry and capacity.
    std::vector<double> stripeLo(nStripes), stripeHi(nStripes), stripeCap(nStripes),
        stripeUsed(nStripes, 0.0);
    const double axisLo = vertical ? g.x0 : g.y0;
    const double dAxis = vertical ? g.dx : g.dy;
    for (std::size_t s = 0; s < nStripes; ++s) {
        stripeLo[s] = axisLo + static_cast<double>(bounds[s]) * dAxis;
        stripeHi[s] = axisLo + static_cast<double>(bounds[s + 1]) * dAxis;
        // Capacity is g * A_a of the stripe, with A_a the AVAILABLE area, not
        // the geometric area. A stripe that is half covered by a fixed macro has
        // half the room; charging it the full geometric area lets the greedy pack
        // drop cells inside the blockage, which is precisely the overlap the
        // legalizer is supposed to remove.
        double sa = 0.0;
        for (std::size_t t = bounds[s]; t < bounds[s + 1]; ++t) {
            for (std::size_t u = b0; u <= b1; ++u) {
                sa += g.avail[vertical ? g.at(t, u) : g.at(u, t)];
            }
        }
        stripeCap[s] = g_ * sa;
    }
    // Furthest stripe from the cutline first, as in Figure 4(iii).
    std::vector<std::size_t> order(nStripes);
    std::iota(order.begin(), order.end(), 0u);
    const auto dist = [&](std::size_t s) {
        return std::abs(0.5 * (stripeLo[s] + stripeHi[s]) - cutCoord);
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t p, std::size_t q) {
        return dist(p) > dist(q);
    });

    // (ii) Cells sorted by distance from C_B, descending: the cell furthest from
    // the cutline is assigned to the furthest stripe.
    std::vector<std::uint32_t> seq = cells;
    const auto cellDist = [&](std::uint32_t i) {
        return std::abs((vertical ? pinX_[i] : pinY_[i]) - cutCoord);
    };
    std::stable_sort(seq.begin(), seq.end(), [&](std::uint32_t a, std::uint32_t b) {
        return cellDist(a) > cellDist(b);
    });

    // (iii) Greedy packing into the furthest stripe that still has room. A stripe
    // is full once it holds g * A_a.
    std::vector<std::vector<std::uint32_t>> packed(nStripes);
    for (const std::uint32_t i : seq) {
        std::size_t chosen = nStripes;
        for (const std::size_t s : order) {
            if (stripeUsed[s] + area_[i] <= stripeCap[s]) {
                chosen = s;
                break;
            }
        }
        if (chosen == nStripes) {
            // Every stripe is at or over its nominal capacity. Put the cell in the
            // one with the most headroom left rather than the nearest to the
            // cutline, so an over-full region evens out instead of piling into one
            // edge. The comparison starts from -inf so a stripe is always picked
            // even when every headroom is zero.
            double best = -std::numeric_limits<double>::max();
            for (const std::size_t s : order) {
                const double head = stripeCap[s] - stripeUsed[s];
                if (head > best) {
                    best = head;
                    chosen = s;
                }
            }
            if (chosen == nStripes) {
                chosen = order.front();
            }
        }
        packed[chosen].push_back(i);
        stripeUsed[chosen] += area_[i];
    }

    // (iv) Cell locations within each stripe are linearly scaled from their
    // current locations. Different stripes get different scale factors, and that
    // is the whole source of the nonlinearity.
    for (std::size_t s = 0; s < nStripes; ++s) {
        if (packed[s].empty()) {
            continue;
        }
        double pLo = std::numeric_limits<double>::max();
        double pHi = -std::numeric_limits<double>::max();
        for (const std::uint32_t i : packed[s]) {
            const double p = vertical ? pinX_[i] : pinY_[i];
            pLo = std::min(pLo, p);
            pHi = std::max(pHi, p);
        }
        // Keep a small inset so cells do not end up exactly on a stripe edge.
        const double inset = 0.05 * (stripeHi[s] - stripeLo[s]);
        const double sLo = stripeLo[s] + inset;
        const double sHi = stripeHi[s] - inset;
        const double span = pHi - pLo;
        for (const std::uint32_t i : packed[s]) {
            double &target = vertical ? pinX_[i] : pinY_[i];
            if (span > degEps_) {
                const double p = (vertical ? pinX_[i] : pinY_[i]) - pLo;
                target = sLo + p * (sHi - sLo) / span;
            } else {
                target = 0.5 * (sLo + sHi);
            }
            // Keep the bin index exact for the blocks that run next.
            std::size_t nx2, ny2;
            grid_.locate(pinX_[i], pinY_[i], nx2, ny2);
            rehome(i, grid_.at(nx2, ny2));
        }
    }
}

void SimplePlacer::Impl::processBlock(const Block &B) {
    if (B.level >= par_.maxLevel) {
        return;  // Algorithm 1 line 8
    }
    // M = movable cells inside the block, gathered from the live bin index. The
    // index is maintained by nonlinearScale() as cells are rehomed, so this is
    // exact and costs only the cells actually in the block.
    std::vector<std::uint32_t> M;
    for (std::size_t iy = B.iy0; iy <= B.iy1 && iy < grid_.nby; ++iy) {
        for (std::size_t ix = B.ix0; ix <= B.ix1 && ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            M.insert(M.end(), binCells_[k].begin(), binCells_[k].end());
        }
    }
    if (M.size() <= par_.minCellsToSplit) {
        return;  // "Area(B) is small enough"
    }
    maxBlockCells_ = std::max(maxBlockCells_, M.size());

    const std::size_t a0 = B.vertical ? B.ix0 : B.iy0;  // extent along the cut axis
    const std::size_t a1 = B.vertical ? B.ix1 : B.iy1;
    const std::size_t b0 = B.vertical ? B.iy0 : B.ix0;  // extent across it
    const std::size_t b1 = B.vertical ? B.iy1 : B.ix1;
    if (a0 >= a1) {
        return;
    }

    // C_c: the cutline that evenly splits the cell area, i.e. the cell-area
    // median along the axis.
    double cellArea = 0.0;
    for (std::size_t t = a0; t <= a1; ++t) {
        for (std::size_t s = b0; s <= b1; ++s) {
            cellArea += grid_.occ[B.vertical ? grid_.at(t, s) : grid_.at(s, t)];
        }
    }
    // C_B: the cutline that evenly partitions the whitespace, i.e. the median of
    // the available area that is *not* occupied.
    double white = 0.0;
    for (std::size_t t = a0; t <= a1; ++t) {
        for (std::size_t s = b0; s <= b1; ++s) {
            const std::size_t k = B.vertical ? grid_.at(t, s) : grid_.at(s, t);
            white += std::max(grid_.avail[k] - grid_.occ[k], 0.0);
        }
    }

    const auto medianCut = [&](double target, bool byCellArea) {
        double cum = 0.0;
        std::size_t cut = a1;
        for (std::size_t t = a0; t <= a1; ++t) {
            for (std::size_t s = b0; s <= b1; ++s) {
                const std::size_t k = B.vertical ? grid_.at(t, s) : grid_.at(s, t);
                cum += byCellArea ? grid_.occ[k] : std::max(grid_.avail[k] - grid_.occ[k], 0.0);
            }
            if (cum >= target) {
                cut = t;
                break;
            }
        }
        return cut;
    };

    std::size_t cutC = (cellArea > 0.0) ? medianCut(0.5 * cellArea, true) : a0 + (a1 - a0) / 2;
    std::size_t cutB = (white > 0.0) ? medianCut(0.5 * white, false) : a0 + (a1 - a0) / 2;
    // Both cutlines must actually separate the block, otherwise the recursion
    // would not make progress.
    cutC = std::clamp(cutC, a0, a1 - 1);
    cutB = std::clamp(cutB, a0, a1 - 1);

    const double axisLo = B.vertical ? grid_.x0 : grid_.y0;
    const double dAxis = B.vertical ? grid_.dx : grid_.dy;
    const double cutCoord = axisLo + static_cast<double>(cutB) * dAxis;

    const std::size_t bA0 = a0, bA1 = cutB, bB0 = cutB + 1, bB1 = a1;

    // (M_0, M_1) come from the cell-area cutline; (B_0, B_1) from the whitespace
    // cutline. Because the two partitions differ, redistributing cells into the
    // whitespace partition is what equalises density across the two halves.
    std::vector<std::uint32_t> M0, M1;
    const double cutCoordC = axisLo + static_cast<double>(cutC + 1) * dAxis;
    for (const std::uint32_t i : M) {
        const double p = (B.vertical ? pinX_[i] : pinY_[i]);
        (p < cutCoordC ? M0 : M1).push_back(i);
    }

    // Rebalance the cell partition against the space partition BEFORE packing.
    //
    // C_c splits the cells by cell-area median and C_B splits the whitespace, and
    // for a collapsed input those partitions are unrelated: one sub-region can be
    // handed far more cell area than the other has available room for. The greedy
    // then saturates every stripe of the over-subscribed side and the under-
    // subscribed side stays empty. The paper's stated purpose for the two-cutline
    // scheme is that it "shifts [C_c] toward the median of available area C_B in
    // the region, so as to equalize densities in the two sub regions"; this pass
    // performs that equalisation in area terms, before any geometry is moved.
    {
        const auto availOf = [&](std::size_t p0, std::size_t p1) {
            double acc = 0.0;
            for (std::size_t t = p0; t <= p1; ++t) {
                for (std::size_t u = b0; u <= b1; ++u) {
                    acc += grid_.avail[B.vertical ? grid_.at(t, u) : grid_.at(u, t)];
                }
            }
            return acc;
        };
        const auto areaOf = [&](const std::vector<std::uint32_t> &cs) {
            double acc = 0.0;
            for (const std::uint32_t i : cs) {
                acc += area_[i];
            }
            return acc;
        };
        const double cap0 = g_ * availOf(bA0, bA1);
        const double cap1 = g_ * availOf(bB0, bB1);
        const double total = areaOf(M0) + areaOf(M1);
        if (cap0 > 0.0 && cap1 > 0.0 && total > 0.0) {
            // Share the cells in proportion to what each side can hold. Cells are
            // handed over NEAREST THE CUTLINE first: both choices move the same
            // area, but crossing the cutline is a short displacement, whereas
            // relocating a few large cells can scramble whole net clusters.
            const double target0 = total * (cap0 / (cap0 + cap1));
            const auto byCutline = [&](const std::vector<std::uint32_t> &src) {
                std::vector<std::uint32_t> o = src;
                std::stable_sort(o.begin(), o.end(), [&](std::uint32_t p, std::uint32_t q) {
                    return std::abs((B.vertical ? pinX_[p] : pinY_[p]) - cutCoord) <
                           std::abs((B.vertical ? pinX_[q] : pinY_[q]) - cutCoord);
                });
                return o;
            };
            double cur0 = areaOf(M0);
            std::vector<std::uint32_t> moved;
            // Which side gives cells away. Exactly one transfer happens, so the
            // M0 -> M1 move below must not also run after the M1 -> M0 branch has
            // already applied its own move (doing both cancels the first out).
            bool fromOne = false;
            if (cur0 > target0) {
                for (const std::uint32_t i : byCutline(M0)) {
                    if (cur0 <= target0) {
                        break;
                    }
                    cur0 -= area_[i];
                    moved.push_back(i);
                }
            } else {
                for (const std::uint32_t i : byCutline(M1)) {
                    if (cur0 >= target0) {
                        break;
                    }
                    cur0 += area_[i];
                    moved.push_back(i);
                }
                std::vector<char> gone(numMovable_, 0);
                for (const std::uint32_t i : moved) {
                    gone[i] = 1;
                }
                std::vector<std::uint32_t> keep;
                for (const std::uint32_t i : M1) {
                    if (!gone[i]) {
                        keep.push_back(i);
                    }
                }
                M1 = keep;
                M0.insert(M0.end(), moved.begin(), moved.end());
                fromOne = true;
            }
            if (cur0 > target0 && !fromOne) {
                std::vector<char> gone(numMovable_, 0);
                for (const std::uint32_t i : moved) {
                    gone[i] = 1;
                }
                std::vector<std::uint32_t> keep;
                for (const std::uint32_t i : M0) {
                    if (!gone[i]) {
                        keep.push_back(i);
                    }
                }
                M0 = keep;
                M1.insert(M1.end(), moved.begin(), moved.end());
            }
        }
    }

    nonlinearScale(M0, bA0, bA1, b0, b1, B.vertical, cutCoord);
    nonlinearScale(M1, bB0, bB1, b0, b1, B.vertical, cutCoord);

    // Alternate the cut direction at each level, and enqueue both whitespace
    // halves.
    Block n0, n1;
    n0.level = n1.level = B.level + 1;
    n0.vertical = n1.vertical = !B.vertical;
    if (B.vertical) {
        n0 = {bA0, bA1, B.iy0, B.iy1, n0.level, n0.vertical};
        n1 = {bB0, bB1, B.iy0, B.iy1, n1.level, n1.vertical};
    } else {
        n0 = {B.ix0, B.ix1, bA0, bA1, n0.level, n0.vertical};
        n1 = {B.ix0, B.ix1, bB0, bB1, n1.level, n1.vertical};
    }
    pending_.push_back(n0);
    pending_.push_back(n1);
}

void SimplePlacer::Impl::lookAheadLegalize() {
    const double before_ = scaledOverflow();
    // 1) Identify g-overfilled bins and cluster them by BFS (4-connected).
    std::vector<char> over(grid_.size(), 0);
    for (std::size_t k = 0; k < grid_.size(); ++k) {
        if (grid_.avail[k] > 0.0 && grid_.occ[k] / grid_.avail[k] > g_) {
            over[k] = 1;
        }
    }
    std::vector<char> seen(grid_.size(), 0);
    std::vector<std::size_t> queue;
    std::size_t nClusters = 0;
    std::size_t globalRegions_ = 0;
    const double totalCellArea_ = grid_.totalCellArea;

    for (std::size_t seed = 0; seed < grid_.size(); ++seed) {
        if (!over[seed] || seen[seed]) {
            continue;
        }
        queue.clear();
        queue.push_back(seed);
        seen[seed] = 1;
        std::size_t ix0 = seed % grid_.nbx, ix1 = ix0;
        std::size_t iy0 = seed / grid_.nbx, iy1 = iy0;
        for (std::size_t h = 0; h < queue.size(); ++h) {
            const std::size_t k = queue[h];
            const std::size_t cx = k % grid_.nbx;
            const std::size_t cy = k / grid_.nbx;
            ix0 = std::min(ix0, cx);
            ix1 = std::max(ix1, cx);
            iy0 = std::min(iy0, cy);
            iy1 = std::max(iy1, cy);
            const int dx4[4] = {1, -1, 0, 0};
            const int dy4[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const long nx = static_cast<long>(cx) + dx4[d];
                const long ny = static_cast<long>(cy) + dy4[d];
                if (nx < 0 || ny < 0 || nx >= static_cast<long>(grid_.nbx) ||
                    ny >= static_cast<long>(grid_.nby)) {
                    continue;
                }
                const std::size_t kk =
                    grid_.at(static_cast<std::size_t>(nx), static_cast<std::size_t>(ny));
                if (over[kk] && !seen[kk]) {
                    seen[kk] = 1;
                    queue.push_back(kk);
                }
            }
        }

        ++nClusters;
        // 3) Grow to a minimal containing rectangle whose density is <= g.
        //
        // The paper asks for the MINIMAL such rectangle, and for a local overfull
        // cluster in an otherwise spread placement that is right: it limits the
        // legalizer's freedom exactly as intended ("overlap removal in a region
        // which is filled to capacity is more straightforward ... the absence of
        // whitespace leaves less flexibility for interconnect optimization").
        //
        // It breaks down when the placement is not spread at all. A collapsed blob
        // is a single cluster whose minimal legal-density rectangle is only as big
        // as the cells strictly need -- about 54% of a 53.5%-utilisation die -- so
        // the top-down partitioning is confined to that sub-rectangle and the rest
        // of the die never receives a cell. Measured: the whole left quarter and
        // the top third sat at zero occupancy with bins at 200% density, and that
        // is the ~9x wirelength penalty, because the I/O pads ring the die so cells
        // trapped in a sub-rectangle are far from half of them. (A "lower bound"
        // with wirelength BELOW the legalized result was the tell -- only possible
        // if the legalized cells overlap.)
        //
        // So the minimal rule is kept for genuinely local clusters, and the whole
        // usable die is used when the cluster already holds most of the movable
        // area, i.e. when the placement is globally collapsed and there is no
        // spread placement left to preserve.
        std::size_t rx0 = ix0, rx1 = ix1, ry0 = iy0, ry1 = iy1;
        double clusterArea = 0.0;
        for (std::size_t iy = ry0; iy <= ry1; ++iy) {
            for (std::size_t ix = rx0; ix <= rx1; ++ix) {
                clusterArea += grid_.occ[grid_.at(ix, iy)];
            }
        }
        if (par_.globalClusterFrac > 0.0 && totalCellArea_ > 0.0 &&
            clusterArea >= par_.globalClusterFrac * totalCellArea_) {
            std::size_t ux0 = grid_.nbx, uy0 = grid_.nby, ux1 = 0, uy1 = 0;
            for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                if (grid_.avail[k] <= 0.0) {
                    continue;
                }
                const std::size_t ix = k % grid_.nbx;
                const std::size_t iy = k / grid_.nbx;
                ux0 = std::min(ux0, ix);
                ux1 = std::max(ux1, ix);
                uy0 = std::min(uy0, iy);
                uy1 = std::max(uy1, iy);
            }
            if (ux0 <= ux1 && uy0 <= uy1) {
                rx0 = ux0;
                rx1 = ux1;
                ry0 = uy0;
                ry1 = uy1;
                ++globalRegions_;
            }
        }
        while (densityOf(rx0, rx1, ry0, ry1) > g_) {
            const bool canL = rx0 > 0;
            const bool canR = rx1 + 1 < grid_.nbx;
            const bool canB = ry0 > 0;
            const bool canT = ry1 + 1 < grid_.nby;
            if (!(canL || canR || canB || canT)) {
                break;  // the whole die is overfull; nothing more to give
            }
            // Expand towards the side that contributes the most available area,
            // ties broken in a fixed order so the result is deterministic.
            //
            // The score is the available area *added*, which is never negative.
            // Scoring the net (available - occupied) instead looks equivalent and
            // is not: the starting bin is overfull by construction, so that
            // quantity is negative there, no side could beat a zero seed, the
            // growth gave up immediately, and every cluster rectangle stayed one
            // bin across -- so the legalizer never had any room to spread into
            // and the placement kept its quadratic-solve degeneracy.
            double bestGain = -1.0;
            int bestSide = -1;
            const bool allowed[4] = {canL, canR, canB, canT};
            double baseAvail = 0.0;
            for (std::size_t iy = ry0; iy <= ry1; ++iy) {
                for (std::size_t ix = rx0; ix <= rx1; ++ix) {
                    baseAvail += grid_.avail[grid_.at(ix, iy)];
                }
            }
            // Only sides that still have room are evaluated -- an expansion past
            // the die would read outside the grid.
            const auto gain = [&](int side) -> double {
                std::size_t a0 = rx0, a1 = rx1, b0 = ry0, b1 = ry1;
                if (side == 0) {
                    --a0;
                } else if (side == 1) {
                    ++a1;
                } else if (side == 2) {
                    --b0;
                } else {
                    ++b1;
                }
                double a = 0.0;
                for (std::size_t iy = b0; iy <= b1; ++iy) {
                    for (std::size_t ix = a0; ix <= a1; ++ix) {
                        a += grid_.avail[grid_.at(ix, iy)];
                    }
                }
                return a - baseAvail;
            };
            for (int side = 0; side < 4; ++side) {
                if (!allowed[side]) {
                    continue;
                }
                const double g = gain(side);
                if (g > bestGain) {
                    bestGain = g;
                    bestSide = side;
                }
            }
            if (bestSide < 0) {
                // No side could be scored (all gains were unusable). Growing
                // anyway would walk the rectangle off the grid, so stop here.
                break;
            }
            if (bestSide == 0) {
                --rx0;
            } else if (bestSide == 1) {
                ++rx1;
            } else if (bestSide == 2) {
                --ry0;
            } else {
                ++ry1;
            }
        }

        pending_.clear();
        pending_.push_back(Block{rx0, rx1, ry0, ry1, 1, true});
        while (!pending_.empty()) {
            const Block B = pending_.front();
            pending_.pop_front();
            ++blocksProcessed_;
            processBlock(B);
            deepestLevel_ = std::max(deepestLevel_, B.level);
        }
    }
    {
        // Where the residual overflow sits: bins overlapped by a fixed macro, or
        // bins with no blockage at all. This separates "the spread did not reach
        // here" from "cells were dropped on top of a macro".
        binCells(pinX_, pinY_);
        double exMacro = 0.0, exFree = 0.0, nMacroBins = 0.0;
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            const double cap = g_ * grid_.avail[k];
            if (grid_.occ[k] <= cap) {
                continue;
            }
            if (grid_.avail[k] < 0.99 * grid_.binArea) {
                exMacro += grid_.occ[k] - cap;
                nMacroBins += 1.0;
            } else {
                exFree += grid_.occ[k] - cap;
            }
        }
        std::size_t onMacro = 0;
        for (std::size_t i = 0; i < numMovable_; ++i) {
            for (const std::uint32_t fv : fixVertex_) {
                const Vertex &fv2 = graph_.getVertex(fv);
                if (pinX_[i] < vx_[fv] + fv2.width && pinX_[i] + areaMovW_[i] > vx_[fv] &&
                    pinY_[i] < vy_[fv] + fv2.height && pinY_[i] + areaMovH_[i] > vy_[fv]) {
                    ++onMacro;
                    break;
                }
            }
        }
        ktlog.trace(
            "  residual: excess on macro bins {:.4g}, on free bins {:.4g} "
            "({:.0f} macro bins); cells overlapping a macro: {}",
            exMacro / std::max(grid_.totalAvail, 1.0), exFree / std::max(grid_.totalAvail, 1.0),
            nMacroBins, onMacro);
    }
    // What the legalizer achieved, in the only terms that matter: the density
    // before and after, plus how deep the top-down partitioning actually got.
    binCells(pinX_, pinY_);
    const double after = scaledOverflow();
    {
        // Column- and row-wise area profile of the legalized result. A
        // symmetric legalizer fills both sides of the die; empty bands show up
        // here as short buckets, and they localise the bias immediately.
        constexpr std::size_t kB = 16;
        std::vector<double> colX(kB, 0.0), rowY(kB, 0.0);
        std::vector<double> colCap(kB, 0.0), rowCap(kB, 0.0);
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            const std::size_t ix = k % grid_.nbx;
            const std::size_t iy = k / grid_.nbx;
            colX[ix * kB / grid_.nbx] += grid_.occ[k];
            colCap[ix * kB / grid_.nbx] += grid_.avail[k];
            rowY[iy * kB / grid_.nby] += grid_.occ[k];
            rowCap[iy * kB / grid_.nby] += grid_.avail[k];
        }
        std::string px, py;
        for (std::size_t i = 0; i < kB; ++i) {
            const double ux = (colCap[i] > 0.0) ? colX[i] / colCap[i] : 0.0;
            const double uy = (rowCap[i] > 0.0) ? rowY[i] / rowCap[i] : 0.0;
            px += " " + std::to_string(static_cast<int>(ux * 9.99));
            py += " " + std::to_string(static_cast<int>(uy * 9.99));
        }
        ktlog.trace("  column utilisation (0-9, left->right):{}", px);
        ktlog.trace("  row    utilisation (0-9, bottom->top):{}", py);
    }
    ktlog.trace(
        "  look-ahead: {} cluster(s) ({} used the whole usable die), {} block(s), "
        "deepest level {}, largest block {} cells; scaled overflow {:.4f} -> {:.4f}",
        nClusters, globalRegions_, blocksProcessed_, deepestLevel_, maxBlockCells_, before_, after);
    {
        // Shape of the resulting density field, not just its total excess: a
        // histogram of per-bin utilisation, and how much of the die is touched
        // at all. An equi-area redistribution should look like a broad hump
        // around the target; a persistent ridge or a lopsided fill shows up here
        // as mass piled in a few utilisation buckets with many bins empty.
        std::size_t empty = 0, touched = 0, overfullBins = 0;
        double worst = 0.0;
        std::size_t hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            if (grid_.avail[k] <= 0.0) {
                continue;
            }
            const double u = grid_.occ[k] / grid_.avail[k];
            if (grid_.occ[k] <= 0.0) {
                ++empty;
            } else {
                ++touched;
            }
            if (u > 1.0) {
                ++overfullBins;
            }
            worst = std::max(worst, u);
            const int b = std::min(7, static_cast<int>(u * 8.0));
            ++hist[std::max(b, 0)];
        }
        std::string buckets;
        for (int i = 0; i < 8; ++i) {
            buckets += " " + std::to_string(hist[i]);
        }
        ktlog.trace(
            "  density shape: worst bin utilisation {:.2f}, {} overfull bins, "
            "{} empty of {} usable bins ({:.0f}% touched)",
            worst, overfullBins, empty, empty + touched,
            100.0 * static_cast<double>(touched) / std::max<double>(empty + touched, 1.0));
        ktlog.trace(
            "  utilisation histogram [0-.125 .125-.25 .25-.375 .375-.5 .5-.625 "
            ".625-.75 .75-1 >1]:{}",
            buckets);
    }
    (void)before_;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::describe(const std::vector<double> &px, const std::vector<double> &py,
                                  const char *tag) const {
    if (numMovable_ == 0) {
        return;
    }
    const double inv = 1.0 / static_cast<double>(numMovable_);
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < numMovable_; ++i) {
        mx += px[i];
        my += py[i];
    }
    mx *= inv;
    my *= inv;
    double vx = 0.0, vy = 0.0, vxy = 0.0;
    double lox = px[0], hix = px[0], loy = py[0], hiy = py[0];
    for (std::size_t i = 0; i < numMovable_; ++i) {
        const double dx = px[i] - mx;
        const double dy = py[i] - my;
        vx += dx * dx;
        vy += dy * dy;
        vxy += dx * dy;
        lox = std::min(lox, px[i]);
        hix = std::max(hix, px[i]);
        loy = std::min(loy, py[i]);
        hiy = std::max(hiy, py[i]);
    }
    vx *= inv;
    vy *= inv;
    vxy *= inv;
    const double sx = std::sqrt(vx) / dieW_;
    const double sy = std::sqrt(vy) / dieH_;
    const double rho = (vx > 0.0 && vy > 0.0) ? vxy / std::sqrt(vx * vy) : 0.0;
    const double fillX = (hix - lox) / dieW_;
    const double fillY = (hiy - loy) / dieH_;
    ktlog.trace(
        "  {}: centroid ({:.3f},{:.3f}) sigma ({:.3f},{:.3f}) rho {:+.3f} "
        "bbox fill {:.2f}x{:.2f}",
        tag, (mx - die_[0]) / dieW_, (my - die_[1]) / dieH_, sx, sy, rho, fillX, fillY);
}

void SimplePlacer::Impl::writeFrame(const std::string &path, const std::vector<double> &px,
                                    const std::vector<double> &py, double hp, double ovf,
                                    const std::string &note, std::size_t step, std::size_t total) {
    std::vector<float> fx(nv_), fy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        fx[v] = static_cast<float>(vx_[v]);
        fy[v] = static_cast<float>(vy_[v]);
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        fx[movVertex_[i]] = static_cast<float>(px[i]);
        fy[movVertex_[i]] = static_cast<float>(py[i]);
    }
    // fixedView: every frame in a sequence is drawn at the same die-relative
    // scale, so iteration N and N+1 are comparable instead of being auto-zoomed.
    writeFrameSvg(path, graph_, fx, fy, die_, step, total, hp, res_.hpwlSeed, ovf, note, nullptr,
                  /*fixedView=*/true);
    ++res_.framesWritten;
}

namespace {

/// Zero-padded step number, so a directory listing sorts in run order instead of
/// alphabetically. Frames are meant to be flipped through in sequence, and
/// "it9" sorting after "it10" breaks that.
std::string frameStep(std::size_t n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) {
        s.insert(s.begin(), '0');
    }
    return s;
}

}  // namespace

void SimplePlacer::Impl::writeCgFrame(const std::string &tag, char dim, std::size_t cgIter,
                                      double resid) {
    // The axis under solve is plotted against the other axis' current iterate.
    // That is the honest picture of a separable solve: x is being refined while
    // y is still wherever the previous solve left it.
    std::vector<float> fx(nv_), fy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        fx[v] = static_cast<float>(vx_[v]);
        fy[v] = static_cast<float>(vy_[v]);
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        fx[movVertex_[i]] = static_cast<float>(solX_[i]);
        fy[movVertex_[i]] = static_cast<float>(solY_[i]);
    }
    // A CG iterate is not a placement worth an HPWL/overflow label, so those are
    // passed as 0 and the residual carries the meaning in the note.
    const std::string path =
        frameDir_ + "/simpl_cg_" + tag + "_" + dim + "_" + frameStep(cgIter) + ".svg";
    writeFrameSvg(
        path, graph_, fx, fy, die_, cgIter, par_.cgMaxIter, 0.0, res_.hpwlSeed, 0.0,
        fmt::format("CG {} iterate {} ({} axis), residual {:.3e}", tag, cgIter, dim, resid),
        nullptr, /*fixedView=*/true);
    ++res_.framesWritten;
}

double SimplePlacer::Impl::binLocal(const std::vector<double> &px, const std::vector<double> &py,
                                    std::vector<double> &occ) const {
    occ.assign(grid_.occ.size(), 0.0);
    for (std::size_t i = 0; i < numMovable_; ++i) {
        std::size_t ix, iy;
        grid_.locate(px[i], py[i], ix, iy);
        occ[grid_.at(ix, iy)] += area_[i];
    }
    double ex = 0.0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        const double cap = g_ * grid_.avail[k];
        if (occ[k] > cap) {
            ex += occ[k] - cap;
        }
    }
    return (grid_.totalAvail > 0.0) ? ex / grid_.totalAvail : 0.0;
}

void SimplePlacer::Impl::writeDensityMap(const std::string &path, const std::vector<double> &px,
                                         const std::vector<double> &py, const std::string &note) {
    std::vector<double> occ;
    const double ovf = binLocal(px, py, occ);

    // One rectangle per bin, coloured by occ/avail. This is the view that answers
    // "is the lower bound actually spreading": a cell scatter plot of 210k cells
    // still looks like a blob at this scale, whereas the density field is
    // readable bin by bin.
    constexpr int W = 900, H = 620, PAD = 52;
    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
        << "\">\n<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<title>" << note << "</title>\n";
    const double sx = (W - 2 * PAD) / dieW_;
    const double sy = (H - 2 * PAD) / dieH_;
    const auto toX = [&](double x) {
        return PAD + (x - die_[0]) * sx;
    };
    const auto toY = [&](double y) {
        return H - PAD - (y - die_[1]) * sy;
    };
    const double bw = std::max(1.0, grid_.dx * sx);
    const double bh = std::max(1.0, grid_.dy * sy);

    for (std::size_t iy = 0; iy < grid_.nby; ++iy) {
        for (std::size_t ix = 0; ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            if (!(grid_.avail[k] > 0.0)) {
                continue;  // no sites at all: not part of the density problem
            }
            const double u = occ[k] / grid_.avail[k];
            // Blue (empty) -> green (at capacity) -> red (overfull). Piecewise
            // linear in u, so the eye reads a fill level directly.
            int r, g, b;
            if (u > 1.0) {
                const double t = std::min((u - 1.0) / 2.0, 1.0);
                r = 255;
                g = static_cast<int>(90.0 * (1.0 - t));
                b = static_cast<int>(90.0 * (1.0 - t));
            } else {
                r = static_cast<int>(40.0 + 90.0 * u);
                g = static_cast<int>(90.0 + 150.0 * u);
                b = static_cast<int>(190.0 * (1.0 - u));
            }
            out << "<rect x=\"" << toX(grid_.binLoX(ix)) << "\" y=\""
                << toY(grid_.binLoY(iy) + grid_.dy) << "\" width=\"" << bw << "\" height=\"" << bh
                << "\" fill=\"rgb(" << r << ',' << g << ',' << b << ")\"/>\n";
        }
    }
    // Fixed macros, outlined, so blockage is distinguishable from legal space.
    for (const std::uint32_t fv : fixVertex_) {
        const Vertex &vert = graph_.getVertex(fv);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        out << "<rect x=\"" << toX(vx_[fv]) << "\" y=\"" << toY(vy_[fv] + vert.height)
            << "\" width=\"" << (vert.width * sx) << "\" height=\"" << (vert.height * sy)
            << "\" fill=\"#bdbdbd\" fill-opacity=\"0.45\" stroke=\"#bdbdbd\" "
               "stroke-width=\"0.5\"/>\n";
    }
    out << "<text x=\"" << PAD
        << "\" y=\"24\" fill=\"#e0e0e0\" font-family=\"monospace\" "
           "font-size=\"14\">bin density "
        << note << "  ovf " << fmt::format("{:.4f}", ovf) << "</text>\n";
    // Legend.
    constexpr int kLegend = 12, kLegendW = 46;
    const int lx0 = PAD, ly0 = H - PAD + 10;
    for (int i = 0; i < kLegend; ++i) {
        const double u = 2.0 * static_cast<double>(i) / (kLegend - 1);  // 0 .. 2
        int r, g, b;
        if (u > 1.0) {
            const double t = std::min((u - 1.0) / 2.0, 1.0);
            r = 255;
            g = static_cast<int>(90.0 * (1.0 - t));
            b = static_cast<int>(90.0 * (1.0 - t));
        } else {
            r = static_cast<int>(40.0 + 90.0 * u);
            g = static_cast<int>(90.0 + 150.0 * u);
            b = static_cast<int>(190.0 * (1.0 - u));
        }
        out << "<rect x=\"" << (lx0 + i * kLegendW) << "\" y=\"" << ly0 << "\" width=\"" << kLegendW
            << "\" height=\"9\" fill=\"rgb(" << r << ',' << g << ',' << b << ")\"/>\n";
    }
    out << "<text x=\"" << lx0 << "\" y=\"" << (ly0 + 22)
        << "\" fill=\"#9e9e9e\" "
           "font-family=\"monospace\" font-size=\"10\">utilisation 0 -> 1 -> 2 (red = "
           "overfull)</text>\n";
    out << "</svg>\n";
    ++res_.framesWritten;
}

void SimplePlacer::Impl::densityStats(const std::vector<double> &px, const std::vector<double> &py,
                                      const char *tag) const {
    std::vector<double> occ;
    const double ovf = binLocal(px, py, occ);
    std::vector<double> util;
    util.reserve(occ.size());
    double worst = 0.0;
    std::size_t worstBin = 0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        if (!(grid_.avail[k] > 0.0)) {
            continue;
        }
        const double u = occ[k] / grid_.avail[k];
        util.push_back(u);
        if (u > worst) {
            worst = u;
            worstBin = k;
        }
    }
    if (util.empty()) {
        return;
    }
    const auto q = [&](double p) {
        std::size_t i = static_cast<std::size_t>(p * static_cast<double>(util.size()));
        std::nth_element(util.begin(), util.begin() + static_cast<long>(i), util.end());
        return util[i];
    };
    double mean = 0.0;
    for (const double u : util) {
        mean += u;
    }
    mean /= static_cast<double>(util.size());
    std::size_t empty = 0, nearCap = 0, overfull = 0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        if (!(grid_.avail[k] > 0.0)) {
            continue;
        }
        if (occ[k] <= 0.0) {
            ++empty;
        }
        const double u = occ[k] / grid_.avail[k];
        if (u > 1.0) {
            ++overfull;
        }
        if (u >= 0.8 && u <= 1.0) {
            ++nearCap;
        }
    }
    // The single worst bin is worth naming precisely: a utilisation of 1e15 can
    // only mean a near-zero available-area bin that still took cells, which is a
    // different defect from an overfull region.
    const double worstAvail = grid_.avail[worstBin];
    ktlog.trace(
        "  density[{}]: ovf {:.4f}, utilisation mean {:.3f} median {:.3f} p99 {:.3f} max {:.4g}; "
        "{} of {} usable bins at 0.8-1.0 ({} overfull, {} empty); worst bin avail {:.4g}",
        tag, ovf, mean, q(0.5), q(0.99), worst, nearCap, util.size(), overfull, empty, worstAvail);
}

// ---------------------------------------------------------------------------

SimplResult SimplePlacer::Impl::run(const SimplParams &P, const std::string &plotDir,
                                    const std::string &snapshotDir) {
    par_ = P;
    // Debugging cap: the legalizer is the expensive part, so a short run is
    // needed to iterate on it. Unset in normal use.
    if (const char *e = std::getenv("KTPLACE_SIMPL_ITERS")) {
        par_.maxIters = static_cast<std::size_t>(std::max(std::atoi(e), 1));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_SEED")) {
        par_.seed = static_cast<std::uint64_t>(std::atoll(e));
    }
    // The paper requires 0 < g < 1 (Section 4.2). At g = 1 every stripe is filled
    // to 100% of its available area, so look-ahead legalization's only solution
    // is to spread cells uniformly over the whole die -- a maximum-entropy
    // spread that destroys the density variation wirelength optimisation wants,
    // and the reason legalization looked so pessimistic. Expose it for sweeps.
    if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY")) {
        par_.densityLimit = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_GLOBAL_CLUSTER")) {
        par_.globalClusterFrac = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_PSEUDONET")) {
        const std::string v = e;
        if (v == "inv") {
            par_.pseudonetLaw = SimplParams::PseudonetLaw::InverseLength;
        } else if (v == "const") {
            par_.pseudonetLaw = SimplParams::PseudonetLaw::ConstantStiffness;
        } else {
            ktlog.fatal("KTPLACE_SIMPL_PSEUDONET must be 'inv' or 'const' (got '{}')", v);
        }
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_START")) {
        const std::string v = e;
        if (v == "input") {
            par_.start = SimplParams::StartPlacement::Input;
        } else if (v == "auto") {
            par_.start = SimplParams::StartPlacement::Auto;
        } else if (v == "uniform") {
            par_.start = SimplParams::StartPlacement::Uniform;
        } else {
            ktlog.fatal("KTPLACE_SIMPL_START must be one of: input, auto, uniform (got '{}')", v);
        }
    }
    // Frame cadence. Per-CG-iteration frames are ~4 MB each on a 210k-cell
    // design, so cgEvery defaults to 0 (off) and is meant for short debug runs.
    if (const char *e = std::getenv("KTPLACE_SIMPL_CG_EVERY")) {
        par_.cgEvery = static_cast<std::size_t>(std::max(std::atoi(e), 0));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY_MAPS")) {
        par_.densityMaps = std::atoi(e) != 0;
    }
    g_ = std::clamp(par_.densityLimit, 0.05, 1.0);

    // Frames go to <plotDir>/simpl, NOT next to the output .pl. snapshotDir is
    // derived from the output path, so a run writing to /tmp used to scatter its
    // frames there and leave the work directory empty. Prefer the plot
    // directory, which is always the work directory, and fall back to snapshotDir
    // only when there is no plot directory at all.
    if (!plotDir.empty()) {
        frameDir_ = plotDir + "/simpl";
    } else if (!snapshotDir.empty()) {
        frameDir_ = snapshotDir;
    } else {
        frameDir_.clear();
    }
    if (!frameDir_.empty() && (par_.traceEvery > 0 || par_.cgEvery > 0)) {
        ensureDir(frameDir_);
    }

    ScopedTimer setupTimer("simpl-setup");
    collect();
    buildGrid(P);
    res_.buildSeconds = setupTimer.elapsedSeconds();
    setupTimer.lap();  // record "simpl-setup" in the shared registry
    if (numMovable_ == 0) {
        return res_;
    }

    // ---- where does the starting placement come from? ------------------------
    //
    // Section 4.1 runs an area-blind quadratic solve because the placer has
    // nothing better to start from. But a Bookshelf .pl or a DEF normally already
    // carries a placement, and it is a real solution: adaptec1 ships one at
    // 9.57e7. Discarding it for a uniform seed throws away the best information
    // available, and the area-blind solve cannot recover it because it collapses
    // the cells into a blob (HPWL 4.2e7 with enormous overlap) which the
    // look-ahead legalizer then has to blow back out.
    //
    // So: adopt the design's own placement when it is usable, and fall back to
    // the paper's uniform seed plus Section 4.1 only when it is not.
    const bool inputUsable = [&] {
        if (par_.start == SimplParams::StartPlacement::Uniform) {
            return false;
        }
        if (par_.start == SimplParams::StartPlacement::Input) {
            return true;
        }
        if (inputX_.size() != numMovable_) {
            return false;
        }
        // A design with no placement ships every cell at the same point, so a
        // degenerate bounding box is the tell. Require the cells to occupy a
        // real part of the die and to be mostly inside it.
        double lo = std::numeric_limits<double>::max();
        double hi = -std::numeric_limits<double>::max();
        double loY = std::numeric_limits<double>::max();
        double hiY = -std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < numMovable_; ++i) {
            lo = std::min(lo, inputX_[i]);
            hi = std::max(hi, inputX_[i]);
            loY = std::min(loY, inputY_[i]);
            hiY = std::max(hiY, inputY_[i]);
        }
        if (!(hi - lo > 0.25 * dieW_) || !(hiY - loY > 0.25 * dieH_)) {
            return false;
        }
        std::size_t inside = 0;
        for (std::size_t i = 0; i < numMovable_; ++i) {
            if (inputX_[i] >= die_[0] && inputX_[i] < die_[0] + dieW_ && inputY_[i] >= die_[1] &&
                inputY_[i] < die_[1] + dieH_) {
                ++inside;
            }
        }
        return static_cast<double>(inside) >= par_.minInputInsideFrac * numMovable_;
    }();


    res_.usedInputPlacement = inputUsable;
    if (inputUsable) {
        pinX_ = inputX_;
        pinY_ = inputY_;
    } else {
        seedUniform(par_.seed);
    }
    res_.hpwlInput = hpwl(pinX_, pinY_);

    ktlog.echo(
        "SimPL: {} movable cells, {} fixed, {} nets; grid {}x{}, available area {:.4g}, "
        "cell area {:.4g} (utilisation {:.1f}%), density limit g = {:.2f}; start {} "
        "(input HPWL {:.6e}{})",
        numMovable_, res_.numFixed, res_.nets, grid_.nbx, grid_.nby, grid_.totalAvail,
        grid_.totalCellArea, 100.0 * grid_.totalCellArea / std::max(grid_.totalAvail, 1e-12), g_,
        inputUsable ? "input placement" : "uniform seed", res_.hpwlInput,
        inputUsable ? "" : (par_.seed == 0 ? "" : fmt::format(", seed {}", par_.seed)));

    std::vector<double> lower = pinX_;
    std::vector<double> lowerY = pinY_;
    std::vector<double> upper;
    std::vector<double> upperY;

    res_.hpwlSeed = hpwl(lower, lowerY);
    // Section 4.1's area-blind quadratic solve, alternating B2B rebuilds until
    // HPWL stops improving. Skipped when the design already supplies a
    // placement: the solve ignores cell areas by design, so it collapses the
    // cells into a blob whose wirelength is meaningless, and the only thing it
    // usefully establishes is the ordering of the cells, which the design's own
    // placement already has.
    if (!inputUsable) {
        double prevHpwl = std::numeric_limits<double>::max();
        for (std::size_t it = 0; it < par_.initMaxIters; ++it) {
            // The B2B model is placement-dependent, so the graph is rebuilt
            // from the current locations before every solve.
            buildB2B(lower, lowerY, 0.0, false);
            solX_ = lower;
            solY_ = lowerY;
            solve("init" + frameStep(it), /*allowFrames=*/false);
            lower = solX_;
            lowerY = solY_;
            res_.initIters = it + 1;
            const double h = hpwl(lower, lowerY);
            densityStats(lower, lowerY, fmt::format("init{}", it).c_str());
            if (par_.traceEvery > 0 && !frameDir_.empty()) {
                // binLocal, not scaledOverflow: the live occupancy in grid_.occ is
                // still describing the previous legalizer pass here.
                std::vector<double> tmpOcc;
                const double ovfInit = binLocal(lower, lowerY, tmpOcc);
                const std::string note =
                    fmt::format("LSS init iteration {} of {}", it, par_.initMaxIters);
                writeFrame(frameDir_ + "/simpl_LSS_init_" + frameStep(it) + ".svg", lower, lowerY,
                           h, ovfInit, note, it, par_.initMaxIters);
                if (par_.densityMaps) {
                    writeDensityMap(frameDir_ + "/simpl_density_init_" + frameStep(it) + ".svg",
                                    lower, lowerY, note);
                }
            }
            ktlog.trace("init iter {:2d}: hpwl {:.6e} cg-resid ok", it, h);
            if (!(h < prevHpwl)) {
                break;  // HPWL stopped improving
            }
            prevHpwl = h;
        }
    }
    res_.hpwlLower = hpwl(lower, lowerY);

    // Diagnostic: apply look-ahead legalization repeatedly to ONE fixed input,
    // with no re-solve in between. This separates two different bugs that have
    // the same symptom -- "the legalizer cannot spread" versus "the loop never
    // accumulates" -- by holding the input fixed and asking whether repeated
    // projection alone converges.
    if (const char *envRounds = std::getenv("KTPLACE_SIMPL_LAL_ONLY")) {
        const int rounds = std::max(std::atoi(envRounds), 1);
        std::vector<double> lx = lower;
        std::vector<double> ly = lowerY;
        double prev = std::numeric_limits<double>::max();
        for (int r = 0; r < rounds; ++r) {
            binCells(lx, ly);
            const double ovfBefore = scaledOverflow();
            (void)ovfBefore;
            blocksProcessed_ = 0;
            deepestLevel_ = 0;
            maxBlockCells_ = 0;
            std::vector<double> sx = pinX_;
            std::vector<double> sy = pinY_;
            pinX_ = lx;
            pinY_ = ly;
            lookAheadLegalize();
            lx = pinX_;
            ly = pinY_;
            pinX_ = sx;
            pinY_ = sy;
            binCells(lx, ly);
            const double ovfAfter = scaledOverflow();
            describe(lx, ly, "LAL-only round");
            // How far did the LAL actually move things, and how full is the
            // result? If the answer is "everything moved a lot and the die is
            // now uniformly ~100% dense", the recursion is over-spreading: at
            // 53.5% utilisation the die can never be full.
            double rms = 0.0, maxd = 0.0;
            double lo = 1e300, hi = -1e300, loY = 1e300, hiY = -1e300;
            for (std::size_t i = 0; i < numMovable_; ++i) {
                const double dx = lx[i] - lower[i];
                const double dy = ly[i] - lowerY[i];
                rms += dx * dx + dy * dy;
                maxd = std::max(maxd, std::hypot(dx, dy));
                lo = std::min(lo, lx[i]);
                hi = std::max(hi, lx[i]);
                loY = std::min(loY, ly[i]);
                hiY = std::max(hiY, ly[i]);
            }
            rms = std::sqrt(rms / std::max<std::size_t>(numMovable_, 1));
            double densSum = 0.0;
            std::size_t densN = 0;
            for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                if (grid_.avail[k] <= 0.0) {
                    continue;
                }
                densSum += grid_.occ[k] / grid_.avail[k];
                ++densN;
            }
            // How concentrated is the input? If the lower bound is collapsed
            // into a tiny area, the legalizer has no choice but to spread it by
            // sqrt(peak density), and that factor is paid straight back in HPWL.
            {
                double dlo = 1e300, dhi = -1e300, dloY = 1e300, dhiY = -1e300;
                for (std::size_t i = 0; i < numMovable_; ++i) {
                    dlo = std::min(dlo, lx[i]);
                    dhi = std::max(dhi, lx[i]);
                    dloY = std::min(dloY, ly[i]);
                    dhiY = std::max(dhiY, ly[i]);
                }
                double peak = 0.0, mean = 0.0;
                std::size_t nb = 0;
                for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                    if (grid_.avail[k] <= 0.0) {
                        continue;
                    }
                    const double dn = grid_.occ[k] / grid_.avail[k];
                    peak = std::max(peak, dn);
                    mean += dn;
                    ++nb;
                }
                double aMin = 1e300, aMax = -1e300, oMax = -1e300, oSum = 0.0;
                std::size_t tiny = 0;
                for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                    aMin = std::min(aMin, grid_.avail[k]);
                    aMax = std::max(aMax, grid_.avail[k]);
                    oMax = std::max(oMax, grid_.occ[k]);
                    oSum += grid_.occ[k];
                    if (grid_.avail[k] > 0.0 && grid_.avail[k] < 1e-6 * aMax) {
                        ++tiny;
                    }
                }
                ktlog.echo(
                    "    density field: avail min {:.6g} max {:.6g} ({} bins below 1e-6 of "
                    "max); occ max {:.6g} sum {:.6g} (cell area {:.6g})",
                    aMin, aMax, tiny, oMax, oSum, grid_.totalCellArea);
                const double spanX = dhi - dlo, spanY = dhiY - dloY;
                ktlog.echo(
                    "    input: bbox x[{:.0f},{:.0f}] y[{:.0f},{:.0f}] = {:.2f}x{:.2f} of "
                    "die; peak bin density {:.2f}, mean {:.3f}, needed linear spread "
                    "{:.2f}x",
                    dlo, dhi, dloY, dhiY, spanX / dieW_, spanY / dieH_, peak,
                    nb ? mean / static_cast<double>(nb) : 0.0, std::sqrt(peak));
            }
            ktlog.echo(
                "    moved: rms {:.1f} max {:.1f} (die {:.0f}x{:.0f}); result bbox "
                "x[{:.0f},{:.0f}] y[{:.0f},{:.0f}]; mean bin density {:.3f}",
                rms, maxd, dieW_, dieH_, lo, hi, loY, hiY,
                densN ? densSum / static_cast<double>(densN) : 0.0);
            prev = ovfAfter;
        }
        static_cast<void>(prev);
        return res_;
    }

    // A negative lower/upper gap is impossible for a genuine lower bound: the
    // upper bound's cells are legalized, so its nets cannot be shorter than the
    // wirelength optimum of the linearised objective. It happens only when the
    // legalized cells overlap, which shortens nets artificially. Refuse to report
    // such a result as progress -- this fired once with a gap of -5.8e6 and a
    // "best" wirelength that was simply overlap being scored as a win.
    bool sawInvalidGap = false;

    // ---- global placement iterations --------------------------------------
    double gapRef = -1.0;
    double bestUpper = std::numeric_limits<double>::max();
    int stale = 0;
    bool converged = false;
    double buildAcc = 0.0;
    double solveAcc = 0.0;
    double spreadAcc = 0.0;
    std::vector<std::pair<double, double>> curve;  // (upper HPWL, lower HPWL)

    for (std::size_t it = 0; it < par_.maxIters && !converged; ++it) {
        res_.globalIters = it + 1;

        // (1) Look-ahead legalization: lower bound -> upper bound.
        //
        // Applied repeatedly until the overflow stops improving. A single pass is
        // not enough once the top-down partitioning is handed the whole usable
        // die: it spreads into the space and leaves holes mid-die. Measured in
        // isolation (KTPLACE_SIMPL_LAL_ONLY) the same projection takes adaptec1
        // from 0.508 to 0.143 in one pass and on to 0.026 in two, so the
        // machinery does converge -- the outer loop was simply never giving it
        // the rounds.
        ScopedTimer lapTimer("simpl-legalize");
        upper = lower;
        upperY = lowerY;
        std::vector<double> keepX = pinX_;
        std::vector<double> keepY = pinY_;
        double prevPassOvf = std::numeric_limits<double>::max();
        for (std::size_t pass = 0; pass < std::max<std::size_t>(par_.lalPasses, 1); ++pass) {
            blocksProcessed_ = 0;
            deepestLevel_ = 0;
            maxBlockCells_ = 0;
            binCells(upper, upperY);
            pinX_ = upper;
            pinY_ = upperY;
            lookAheadLegalize();
            upper = pinX_;
            upperY = pinY_;
            pinX_ = keepX;
            pinY_ = keepY;
            binCells(upper, upperY);
            const double ovfNow = scaledOverflow();
            if (!(ovfNow < prevPassOvf * (1.0 - par_.lalMinGain))) {
                break;  // no worthwhile progress from another pass
            }
            prevPassOvf = ovfNow;
        }
        spreadAcc += lapTimer.elapsedSeconds();
        lapTimer.lap();

        binCells(upper, upperY);
        const double upperOvf = scaledOverflow();
        binCells(lower, lowerY);
        describe(lower, lowerY, "LSS (linear system solve)");
        describe(upper, upperY, "LAL (look-ahead legalized)");
        densityStats(lower, lowerY, ("LSS it" + std::to_string(it)).c_str());
        densityStats(upper, upperY, ("LAL it" + std::to_string(it)).c_str());
        const double upperHpwl = hpwl(upper, upperY);
        const double lowerHpwl = hpwl(lower, lowerY);
        const double gap = upperHpwl - lowerHpwl;
        if (gap < 0.0) {
            if (!sawInvalidGap) {
                ktlog.echo(
                    "iter {}: WARNING gap {:.4e} is negative -- the legalized cells overlap, "
                    "so this wirelength is not a legal result and is not counted as progress",
                    it, gap);
                sawInvalidGap = true;
            }
        }
        curve.emplace_back(upperHpwl, lowerHpwl);
        res_.gap = gap;

        if (it == par_.gapReferenceIter) {
            gapRef = gap;
        }
        if (upperHpwl < bestUpper - 1e-12) {
            bestUpper = upperHpwl;
            stale = 0;
        } else {
            ++stale;
        }

        ktlog.trace(
            "iter {:3d}: lower {:.6e} upper {:.6e} gap {:.4e} (ref {:.4e}) "
            "ovf lower {:.4e} upper {:.4e} alpha {:.4g} stale {}",
            it, lowerHpwl, upperHpwl, gap, gapRef, scaledOverflow(), upperOvf,
            par_.alphaBase * (1.0 + static_cast<double>(it)), stale);

        // Convergence. HPWL of the upper bounds oscillates for the first several
        // iterations, so the gap -- not the upper-bound HPWL alone -- is what is
        // watched, referenced to the gap at iteration 10.
        if (gapRef > 0.0 && it > par_.gapReferenceIter) {
            if (gap < par_.gapTightFrac * gapRef) {
                converged = true;
            } else if (gap < par_.gapRelaxedFrac * gapRef &&
                       stale >= static_cast<int>(par_.patience)) {
                converged = true;
            }
        }

        if (par_.traceEvery > 0 && (it % par_.traceEvery) == 0 && !frameDir_.empty()) {
            const std::string step = frameStep(it);
            writeFrame(frameDir_ + "/simpl_LSS_" + step + ".svg", lower, lowerY, lowerHpwl,
                       upperOvf, "LSS (linear system solve) - iteration " + std::to_string(it), it,
                       par_.maxIters);
            writeFrame(frameDir_ + "/simpl_LAL_" + step + ".svg", upper, upperY, upperHpwl,
                       upperOvf, "LAL (look-ahead legalized) - iteration " + std::to_string(it), it,
                       par_.maxIters);
            if (par_.densityMaps) {
                writeDensityMap(frameDir_ + "/simpl_density_LSS_" + step + ".svg", lower, lowerY,
                                "LSS iteration " + std::to_string(it));
                writeDensityMap(frameDir_ + "/simpl_density_LAL_" + step + ".svg", upper, upperY,
                                "LAL iteration " + std::to_string(it));
            }
        }

        if (converged) {
            ktlog.trace("converged at iteration {}: gap {:.4e} vs reference {:.4e}", it, gap,
                        gapRef);
            break;
        }

        // (2) Update anchors and the B2B net model from the upper bound, then
        // (3) re-solve. The pseudonet weight alpha grows with the iteration
        // number, moving the emphasis from interconnect onto constraints.
        anchorX_ = upper;
        anchorY_ = upperY;
        const double alpha = par_.alphaBase * (1.0 + static_cast<double>(it));
        ScopedTimer bTimer("simpl-build");
        // The B2B model and the pseudonet lengths are both measured at the LOWER
        // bound: that is the point being linearised, and the anchor distance is
        // |upper - lower|. Handing buildB2B the upper bound as the current
        // position as well made every pseudonet length identically zero, so the
        // degenerate-length guard discarded all 210k of them and the anchors
        // contributed nothing at all (measured: anchor diagonal exactly 0 against
        // an interconnect diagonal of 6.9e5). With no anchors in the system the
        // solve was pure interconnect minimisation, which is why the lower bound
        // never moved, why lower-bound HPWL only drifted, and why every pass
        // re-legalised the same collapsed input to the same result.
        buildB2B(lower, lowerY, alpha, true);
        buildAcc += bTimer.elapsedSeconds();
        bTimer.lap();
        ScopedTimer sTimer("simpl-solve");
        // Warm start from the previous lower bound. Starting instead from the
        // anchors was tried and rejected: it improved density (adaptec2 overflow
        // 0.093 -> 0.081) but left the final wirelength unchanged (1.73e9 ->
        // 1.75e9) and turned the bound gap negative. The seed is not the lever
        // here; the anchor weight is. See the "pseudonet share" trace, which is
        // where the real diagnosis lives.
        solX_ = lower;
        solY_ = lowerY;
        solve("g" + frameStep(it), /*allowFrames=*/true);
        lower = solX_;
        lowerY = solY_;
        solveAcc += sTimer.elapsedSeconds();
        sTimer.lap();
    }

    if (upper.empty()) {
        // Degenerate case: no iteration completed, fall back to the lower bound.
        upper = lower;
        upperY = lowerY;
    }
    res_.hpwlLower = hpwl(lower, lowerY);
    res_.hpwlFinal = hpwl(upper, upperY);
    res_.gap = res_.hpwlFinal - res_.hpwlLower;
    res_.spreadSeconds = spreadAcc;
    res_.buildSeconds += buildAcc;
    res_.solveSeconds = solveAcc;
    binCells(lower, lowerY);
    res_.overflowLower = scaledOverflow();
    binCells(upper, upperY);
    res_.overflowFinal = scaledOverflow();

    // The result is the last upper bound (Figure 2: "Last Upper-bound
    // Placement"); positions are written back for the normal output path.
    for (std::size_t i = 0; i < numMovable_; ++i) {
        db_.setCellPosition(movVertex_[i], upper[i], upperY[i]);
    }

    if (!plotDir.empty()) {
        ensureDir(plotDir);
        std::ofstream csv(plotDir + "/simpl_bounds.csv");
        csv << "iter,hpwl_upper,hpwl_lower,gap\n";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            csv << i << ',' << curve[i].first << ',' << curve[i].second << ','
                << (curve[i].first - curve[i].second) << '\n';
        }
        // Lower and upper HPWL against iteration: the two bounds meeting is the
        // whole convergence story, so it is worth a picture.
        double mx = 0.0;
        for (const auto &p : curve) {
            mx = std::max({mx, p.first, p.second});
        }
        const int W = 780, H = 340, PAD = 60;
        std::ofstream svg(plotDir + "/simpl_bounds.svg");
        svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
            << "'>\n<rect width='100%' height='100%' fill='white'/>\n"
            << "<text x='" << PAD
            << "' y='24' font-family='monospace' font-size='14'>"
               "SimPL: HPWL of the lower and upper bounds</text>\n";
        const auto px = [&](std::size_t i) {
            return PAD + (W - 2 * PAD) *
                             (curve.size() < 2
                                  ? 0.0
                                  : static_cast<double>(i) / static_cast<double>(curve.size() - 1));
        };
        const auto py = [&](double v) {
            return H - PAD - (H - 2 * PAD) * std::clamp(v / std::max(mx, 1e-12), 0.0, 1.0);
        };
        svg << "<line x1='" << PAD << "' y1='" << (H - PAD) << "' x2='" << (W - PAD) << "' y2='"
            << (H - PAD) << "' stroke='#333'/>\n<line x1='" << PAD << "' y1='" << PAD << "' x2='"
            << PAD << "' y2='" << (H - PAD) << "' stroke='#333'/>\n";
        svg << "<polyline fill='none' stroke='#2471a3' stroke-width='2' points='";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            svg << px(i) << ',' << py(curve[i].second) << ' ';
        }
        svg << "'/><polyline fill='none' stroke='#c0392b' stroke-width='2' points='";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            svg << px(i) << ',' << py(curve[i].first) << ' ';
        }
        svg << "'/>\n<text x='" << (W - PAD - 130) << "' y='" << (PAD + 12)
            << "' font-family='monospace' font-size='12' fill='#2471a3'>lower bound</text>\n"
            << "<text x='" << (W - PAD - 130) << "' y='" << (PAD + 28)
            << "' font-family='monospace' font-size='12' fill='#c0392b'>upper "
               "bound</text>\n</svg>\n";
    }

    if (!frameDir_.empty()) {
        ensureDir(frameDir_);
        writeFrame(frameDir_ + "/simpl_FINAL_LAL.svg", upper, upperY, res_.hpwlFinal,
                   res_.overflowFinal, "FINAL = last LAL (look-ahead legalized) placement",
                   res_.globalIters, std::max<std::size_t>(res_.globalIters, 1));
        if (par_.densityMaps) {
            writeDensityMap(frameDir_ + "/simpl_density_FINAL.svg", upper, upperY, "FINAL LAL");
            writeDensityMap(frameDir_ + "/simpl_density_FINAL_LSS.svg", lower, lowerY, "FINAL LSS");
        }
    }

    ktlog.trace("frames written: {} to {}", res_.framesWritten,
                frameDir_.empty() ? "(none)" : frameDir_);
    return res_;
}

// ---------------------------------------------------------------------------

SimplePlacer::SimplePlacer(PlacementDB &db) : pImpl(std::make_unique<Impl>(db)) {}
SimplePlacer::~SimplePlacer() = default;
SimplePlacer::SimplePlacer(SimplePlacer &&) noexcept = default;
SimplePlacer &SimplePlacer::operator=(SimplePlacer &&) noexcept = default;

SimplResult SimplePlacer::place(const SimplParams &params, const std::string &plotDir,
                                const std::string &snapshotDir) {
    return pImpl->run(params, plotDir, snapshotDir);
}

}  // namespace ktplace
