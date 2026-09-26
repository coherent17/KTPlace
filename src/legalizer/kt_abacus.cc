/**
 * @file kt_abacus.cc
 * @brief Abacus legalization, DP-over-clusters form. See kt_abacus.h.
 */

#include "legalizer/kt_abacus.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>

namespace ktplace {

namespace {

using Clock = std::chrono::steady_clock;

/// Undo information for one place() call on a row.
///
/// Candidate rows are scored by inserting the cell, reading off the cost delta,
/// and undoing it. Snapshotting the row instead would deep-copy every cluster's
/// member vector on every candidate, which cost about 1e8 heap allocations on
/// adaptec1. Every mutation place() makes is O(1) to record, so the rollback is
/// O(1) in the number of clusters it touched.
struct RowUndo {
    std::size_t savedSize = 0;
    bool merged = false;
    std::size_t mergeIdx = 0;
    std::size_t mergePos = 0;
    double savedW = 0.0, savedN = 0.0, savedA = 0.0, savedB = 0.0, savedX = 0.0;
    double savedCost = 0.0;
    std::vector<std::pair<std::size_t, double>> moved;
};

/// Cost of a candidate that does not fit; no real cost reaches this.
constexpr double kInfeasible = std::numeric_limits<double>::infinity();

/// Cells closer than this many site widths to their neighbour are candidates to
/// be merged into one cluster. Abacus uses a small constant gap here; clustering
/// is what stops a row's cells from being dragged apart one at a time.
double clusterGapSites() {
    const char *e = std::getenv("KTPLACE_ABACUS_CLUSTER_GAP");
    return e ? std::atof(e) : 0.0;
}

/// A run of adjacent cells treated as one placeable item.
///
/// Abacus lets neighbouring cells form a cluster that the row's dynamic program
/// then places as a single unit, which is what stops the row's cells from being
/// nudged apart one at a time. Members are laid out *contiguously* inside the
/// cluster at cumulative-width offsets -- not at their original offsets, which
/// after global placement are frequently negative and would leave the cells
/// overlapping. Laying them out contiguously is what makes the cluster
/// internally legal by construction.
///
/// With off_m the cumulative offset of member m, the cluster cost is
///     sum_m (x + off_m - target_m)^2 = n*x^2 + 2*A*x + B
/// so it evaluates in constant time and the ideal left edge is -A/n.
struct Cluster {
    std::vector<std::size_t> members;  ///< movable slots, ascending target x
    double w = 0.0;                    ///< total width == the space reserved
    double x = 0.0;                    ///< committed left edge
    double n = 0.0;                    ///< member count
    double a = 0.0;                    ///< sum of (off_m - target_m)
    double b = 0.0;                    ///< sum of (off_m - target_m)^2

    [[nodiscard]] double costAt(double x) const { return n * x * x + 2.0 * a * x + b; }
    [[nodiscard]] double ideal() const { return (n > 0.0) ? (-a / n) : 0.0; }
};

/// One placement row, cut into the x-intervals that fixed cells leave free.
/// Named RowTrack because datamodel/kt_dm.h forward-declares a stale `class Row`.
struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double siteWidth = 1.0;
    double xlo = 0.0;
    double xhi = 0.0;
    std::vector<std::pair<double, double>> free;  ///< disjoint, sorted
    std::vector<Cluster> clusters;
    /// Committed sum of squared displacement over every cluster in the row.
    double cost = 0.0;

    [[nodiscard]] double widestFree() const {
        double best = 0.0;
        for (const auto &iv : free) {
            best = std::max(best, iv.second - iv.first);
        }
        return best;
    }

    /// Smallest site-aligned x >= `from` with [x, x + w] inside one free
    /// interval. Snapped forward, because a free interval usually starts at a
    /// macro edge, which is generally not a site. NaN if the cell does not fit.
    [[nodiscard]] double firstFitAtLeast(double from, double w) const {
        for (const auto &iv : free) {
            if (iv.second <= from) {
                continue;
            }
            double enter = std::max(from, iv.first);
            if (!(siteWidth > 0.0)) {
                if (iv.second - enter >= w - 1e-9) {
                    return enter;
                }
                continue;
            }
            enter = xlo + std::ceil((enter - xlo) / siteWidth - 1e-9) * siteWidth;
            if (enter + w <= iv.second + 1e-9) {
                return enter;
            }
        }
        return std::numeric_limits<double>::quiet_NaN();
    }
};

}  // namespace

// ---------------------------------------------------------------------------

class AbacusLegalizer::Impl {
public:
    explicit Impl(PlacementDB &db) : db_(db), graph_(db.getGraph()) {}

    LegalizeResult run(const LegalizeParams &params);

private:
    void buildRows();
    /// Insert slot `i` into row `r` and re-place the affected clusters.
    /// Returns the row's cost delta, or kInfeasible. `xsOut` receives the
    /// clusters' new left edges for the suffix that moved.
    /// Insert slot `i` into row `r` and re-place the affected clusters,
    /// returning the row's cost delta or kInfeasible. Only the row's cluster
    /// list and cost are touched; cell positions are not written here, so an
    /// unused candidate leaves nothing behind. `undo` records enough to roll
    /// the row back in time proportional to what actually moved.
    double place(RowTrack &r, std::size_t i, RowUndo &undo);
    void rollback(RowTrack &r, const RowUndo &undo);
    [[nodiscard]] double hpwlOf(const std::vector<double> &x, const std::vector<double> &y) const;
    void writeFrame(const std::string &path, const std::string &note, std::size_t step,
                    std::size_t total) const;
    void selfCheck(LegalizeResult &res) const;

    static constexpr std::size_t kNoRow = std::numeric_limits<std::size_t>::max();

    PlacementDB &db_;
    Graph &graph_;
    std::vector<RowTrack> rows_;
    std::vector<std::size_t> mov_;     ///< graph vertex id per movable slot
    std::vector<double> w_, h_;         ///< per movable slot
    std::vector<double> x0_, y0_;       ///< pre-legalization position (the target)
    std::vector<double> xs_, ys_;       ///< live position
    std::unordered_map<std::size_t, std::size_t> slotOf_;
    BBox die_ = {0.0, 0.0, 0.0, 0.0};
};

void AbacusLegalizer::Impl::buildRows() {
    rows_.clear();
    for (const PlacementDB::RowInfo &ri : db_.getRows()) {
        RowTrack r;
        r.y = ri.coordinate;
        r.height = ri.height;
        r.siteWidth = (ri.sitespacing > 0.0) ? ri.sitespacing : ri.sitewidth;
        if (!(r.siteWidth > 0.0)) {
            r.siteWidth = 1.0;
        }
        r.xlo = ri.xlo();
        r.xhi = ri.xhi();
        if (!(r.xhi > r.xlo)) {
            continue;
        }
        rows_.push_back(r);
    }
    std::sort(rows_.begin(), rows_.end(),
              [](const RowTrack &a, const RowTrack &b) { return a.y < b.y; });
    if (rows_.empty()) {
        return;
    }

    // Fixed cells (macros) cut each row into free intervals.
    struct FixedBox {
        double y0, y1, x0, x1;
    };
    std::vector<FixedBox> fixed;
    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isFixed) {
            continue;
        }
        fixed.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
    }
    std::sort(fixed.begin(), fixed.end(),
              [](const FixedBox &a, const FixedBox &b) { return a.x0 < b.x0; });

    for (RowTrack &r : rows_) {
        std::vector<std::pair<double, double>> hit;
        for (const FixedBox &f : fixed) {
            if (f.y1 <= r.y + 1e-9 || f.y0 >= r.y + r.height - 1e-9) {
                continue;
            }
            const double a = std::max(f.x0, r.xlo);
            const double b = std::min(f.x1, r.xhi);
            if (b > a) {
                hit.emplace_back(a, b);
            }
        }
        r.free.clear();
        double cur = r.xlo;
        for (const auto &iv : hit) {
            if (iv.second <= cur) {
                continue;
            }
            if (iv.first > cur) {
                r.free.emplace_back(cur, iv.first);
            }
            cur = std::max(cur, iv.second);
        }
        if (cur < r.xhi) {
            r.free.emplace_back(cur, r.xhi);
        }
    }
}

double AbacusLegalizer::Impl::place(RowTrack &r, std::size_t i, RowUndo &undo) {
    // Candidate rows are scored by running this and then rolling back, so the
    // row is snapshotted first. Clusters are whole rows' worth of groups, and
    // the early stop below keeps the mutated suffix short, so a snapshot is a
    // bounded cost; it is far cheaper than the alternative, which is letting a
    // rejected candidate's mutations leak into the next candidate.
    undo = RowUndo{};
    undo.savedSize = r.clusters.size();
    undo.savedCost = r.cost;

    const double target = x0_[i];

    // 1. Where the cell lands in the row's cluster order, and whether it can
    //    join a neighbour. Groups are within kClusterGapSites of each other in
    //    the target placement, so a new cell almost always joins one and the
    //    number of placeable items per row stays small.
    std::size_t at = 0;
    while (at < r.clusters.size() && r.clusters[at].x <= target) {
        ++at;
    }
    const double gap = clusterGapSites() * r.siteWidth;
    std::size_t merge = r.clusters.size();
    double bestGap = std::numeric_limits<double>::max();
    if (at < r.clusters.size()) {
        const double g = r.clusters[at].x - target;
        if (g <= gap && g < bestGap) {
            bestGap = g;
            merge = at;  // join the cluster on the right
        }
    }
    if (at > 0) {
        const double g = target - (r.clusters[at - 1].x + r.clusters[at - 1].w);
        if (g <= gap && g < bestGap) {
            bestGap = g;
            merge = at - 1;  // join the cluster on the left
        }
    }

    if (merge < r.clusters.size()) {
        // 2a. Absorb into the neighbouring cluster and re-place from there.
        at = merge;
        Cluster &c = r.clusters[merge];
        const auto pos = static_cast<std::size_t>(
            std::lower_bound(c.members.begin(), c.members.end(), i,
                             [&](std::size_t p, std::size_t q) {
                                 return x0_[p] < x0_[q];
                             }) -
            c.members.begin());
        undo.merged = true;
        undo.mergeIdx = merge;
        undo.mergePos = pos;
        undo.savedW = c.w;
        undo.savedN = c.n;
        undo.savedA = c.a;
        undo.savedB = c.b;
        undo.savedX = c.x;
        c.members.insert(c.members.begin() + static_cast<long>(pos), i);
    } else {
        // 2b. Standalone cluster.
        Cluster c;
        c.members.push_back(i);
        r.clusters.insert(r.clusters.begin() + static_cast<long>(at), std::move(c));
    }

    // Recompute the touched cluster's sums for its new membership. The other
    // clusters keep the sums they were built with.
    {
        Cluster &c = r.clusters[at];
        c.n = static_cast<double>(c.members.size());
        c.w = 0.0;
        c.a = 0.0;
        c.b = 0.0;
        double off = 0.0;
        for (const std::size_t m : c.members) {
            const double d = off - x0_[m];
            c.a += d;
            c.b += d * d;
            off += w_[m];
        }
        c.w = off;
    }

    // 3. Re-place clusters from the touched one rightwards. The greedy is
    //    monotone, so the first cluster that holds its position proves every
    //    cluster to its right is unchanged too, and the loop stops there. That
    //    early stop is what makes the pass near-linear instead of O(row length)
    //    for every cell.
    double cursor = (at > 0) ? (r.clusters[at - 1].x + r.clusters[at - 1].w) : r.xlo;
    double delta = 0.0;
    std::vector<std::size_t> moved;
    for (std::size_t k = at; k < r.clusters.size(); ++k) {
        Cluster &c = r.clusters[k];
        const double oldX = c.x;
        const double oldCost = c.costAt(oldX);

        double ideal = std::max(c.ideal(), cursor);
        const double hi = r.xhi - c.w;  // clamp: a target can sit outside
        if (hi >= r.xlo) {                 // the core after global placement
            ideal = std::min(ideal, hi);
        }
        ideal = std::max(ideal, r.xlo);
        if (r.siteWidth > 0.0) {
            ideal = r.xlo + std::round((ideal - r.xlo) / r.siteWidth) * r.siteWidth;
        }
        const double nx = r.firstFitAtLeast(ideal, c.w);
        if (std::isnan(nx)) {
            rollback(r, undo);  // the row cannot hold this group
            return kInfeasible;
        }
        if (k > at && std::fabs(nx - oldX) < 1e-12) {
            break;  // nothing further right can move either
        }
        delta += c.costAt(nx) - oldCost;
        undo.moved.emplace_back(k, oldX);  // the greedy is monotone, so a
        c.x = nx;                          // cluster that holds also pins the
        cursor = nx + c.w;                 // ones to its right
        moved.push_back(k);
    }
    if (!std::isfinite(delta)) {
        rollback(r, undo);
        return kInfeasible;
    }
    r.cost += delta;
    return delta;
}

void AbacusLegalizer::Impl::rollback(RowTrack &r, const RowUndo &undo) {
    r.cost = undo.savedCost;
    if (undo.merged) {
        Cluster &c = r.clusters[undo.mergeIdx];
        if (undo.mergePos < c.members.size()) {
            c.members.erase(c.members.begin() + static_cast<long>(undo.mergePos));
        }
        c.w = undo.savedW;
        c.n = undo.savedN;
        c.a = undo.savedA;
        c.b = undo.savedB;
        c.x = undo.savedX;
    } else if (r.clusters.size() > undo.savedSize) {
        r.clusters.resize(undo.savedSize);
    }
    for (const auto &mv : undo.moved) {
        r.clusters[mv.first].x = mv.second;
    }
}

double AbacusLegalizer::Impl::hpwlOf(const std::vector<double> &x,
                                     const std::vector<double> &y) const {
    double total = 0.0;
    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Net || vert.inEdges.empty()) {
            continue;
        }
        double ax = std::numeric_limits<double>::max(), bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max(), by = -std::numeric_limits<double>::max();
        for (const std::size_t eid : vert.inEdges) {
            const Edge &e = graph_.getEdge(eid);
            const auto it = slotOf_.find(e.source);
            double cx = 0.0, cy = 0.0;
            if (it != slotOf_.end()) {
                cx = x[it->second] + e.offsetX;
                cy = y[it->second] + e.offsetY;
            } else {
                const Vertex &c = graph_.getVertex(e.source);
                cx = c.x + e.offsetX;
                cy = c.y + e.offsetY;
            }
            ax = std::min(ax, cx);
            bx = std::max(bx, cx);
            ay = std::min(ay, cy);
            by = std::max(by, cy);
        }
        total += (bx - ax) + (by - ay);
    }
    return total;
}

void AbacusLegalizer::Impl::writeFrame(const std::string &path, const std::string &note,
                                       std::size_t step, std::size_t total) const {
    const std::size_t nv = graph_.getNumVertices();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        fx[v] = static_cast<float>(vert.x);
        fy[v] = static_cast<float>(vert.y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(xs_[i]);
        fy[mov_[i]] = static_cast<float>(ys_[i]);
    }
    writeFrameSvg(path, graph_, fx, fy, die_, step, total, hpwlOf(xs_, ys_), 0.0, 0.0, note,
                  nullptr, /*fixedView=*/true);
}

void AbacusLegalizer::Impl::selfCheck(LegalizeResult &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    res.outOfRows = 0;

    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const RowTrack *inRow = nullptr;
        for (const RowTrack &R : rows_) {
            if (ys_[i] >= R.y - eps && ys_[i] <= R.y + R.height + eps) {
                inRow = &R;
                break;
            }
        }
        if (inRow == nullptr) {
            ++res.offRow;
            ++res.outOfRows;
            continue;
        }
        const double rel = xs_[i] - inRow->xlo;
        const double q = rel / inRow->siteWidth;
        const double off = std::fabs(q - std::round(q)) * inRow->siteWidth;
        if (off > 1e-6 * std::max(1.0, std::fabs(rel))) {
            ++res.offSite;
        }
        if (xs_[i] < inRow->xlo - eps || xs_[i] + w_[i] > inRow->xhi + eps) {
            ++res.outOfRows;
        }
    }

    // Overlap: sort every movable cell by (row, x) and test only adjacent pairs.
    // Comparing each row's cells pairwise is quadratic and was both slow and,
    // before the early-stop fix, a source of phantom overlaps.
    std::vector<std::uint64_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
    // Attribute each cell to the row whose band contains it, so a cell left
    // unplaced (still at its global-placement y) is grouped with the row it
    // happens to sit in rather than being lumped into row 0.
    std::vector<std::size_t> rowOf(mov_.size(), 0);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        for (std::size_t r = 0; r < rows_.size(); ++r) {
            if (ys_[i] >= rows_[r].y - eps && ys_[i] <= rows_[r].y + rows_[r].height + eps) {
                rowOf[i] = r;
                break;
            }
        }
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (rowOf[a] != rowOf[b]) {
            return rowOf[a] < rowOf[b];
        }
        return xs_[a] < xs_[b];
    });
    for (std::size_t k = 1; k < order.size(); ++k) {
        const std::size_t a = order[k - 1], b = order[k];
        if (rowOf[a] == rowOf[b] && xs_[a] + w_[a] > xs_[b] + eps) {
            ++res.overlappingPairs;
        }
    }

    // Movable-vs-fixed, x-sorted sweep.
    struct FixedBox {
        double y0, y1, x0, x1;
    };
    std::vector<FixedBox> fixed;
    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isFixed) {
            continue;
        }
        fixed.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
    }
    std::sort(fixed.begin(), fixed.end(),
              [](const FixedBox &a, const FixedBox &b) { return a.x0 < b.x0; });
    std::vector<std::size_t> byX(mov_.size());
    std::iota(byX.begin(), byX.end(), 0u);
    std::sort(byX.begin(), byX.end(),
              [&](std::size_t a, std::size_t b) { return xs_[a] < xs_[b]; });
    std::size_t lo = 0;
    for (const std::size_t i : byX) {
        while (lo < fixed.size() && fixed[lo].x1 <= xs_[i] + eps) {
            ++lo;
        }
        for (std::size_t f = lo; f < fixed.size() && fixed[f].x0 < xs_[i] + w_[i]; ++f) {
            if (fixed[f].x1 <= xs_[i] + eps) {
                continue;  // sorted by x0, so a later box can still end earlier
            }
            if (ys_[i] + h_[i] > fixed[f].y0 + eps && ys_[i] < fixed[f].y1 - eps) {
                ++res.overFixed;
            }
        }
    }
}

LegalizeResult AbacusLegalizer::Impl::run(const LegalizeParams &params) {
    LegalizeResult res;
    const auto t0 = Clock::now();

    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        const std::size_t slot = mov_.size();
        mov_.push_back(v);
        slotOf_[v] = slot;
        w_.push_back(vert.width);
        h_.push_back(vert.height);
        x0_.push_back(vert.x);
        y0_.push_back(vert.y);
    }
    xs_ = x0_;
    ys_ = y0_;
    res.cellsPlaced = mov_.size();
    if (mov_.empty()) {
        return res;
    }
    res.hpwlBefore = hpwlOf(xs_, ys_);

    buildRows();
    if (rows_.empty()) {
        res.unplaced = mov_.size();
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }
    const BBox d = fixedCellBBox(graph_);
    die_ = BBox{d[0], d[1], d[2], d[3]};

    if (!params.plotDir.empty()) {
        std::filesystem::create_directories(params.plotDir);
        writeFrame(params.plotDir + "/legalize_000000.svg", "input placement", 0, mov_.size());
    }

    // Cells are legalized in order of target y, so rows fill from one side and
    // each row's DP sees cells in roughly the order they will sit.
    std::vector<std::size_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t a, std::size_t b) { return y0_[a] < y0_[b]; });

    std::size_t placed = 0;

    for (const std::size_t i : order) {
        std::size_t home = 0;
        double bestD = std::numeric_limits<double>::max();
        for (std::size_t r = 0; r < rows_.size(); ++r) {
            const RowTrack &R = rows_[r];
            const double dd = (y0_[i] < R.y)              ? (R.y - y0_[i])
                              : (y0_[i] > R.y + R.height) ? (y0_[i] - (R.y + R.height))
                                                           : 0.0;
            if (dd < bestD) {
                bestD = dd;
                home = r;
            }
        }

        // Abacus scores a candidate row by the *incremental* cost of the
        // insertion, plus the cell's vertical displacement. That quantity is
        // small and local, which is what makes the search cheap and what keeps
        // cells near their target row: the vertical term is a true lower bound,
        // so once it exceeds the incumbent no farther row can win.
        std::size_t bestRow = kNoRow;
        double bestCost = std::numeric_limits<double>::max();

        const auto tryRow = [&](std::size_t ri) {
            if (ri >= rows_.size() || ri == bestRow) {
                return;
            }
            RowTrack &R = rows_[ri];
            if (h_[i] > R.height + 1e-9 || w_[i] > R.widestFree() + 1e-9) {
                return;
            }
            const double vy = (ys_[i] > R.y + R.height) ? (ys_[i] - (R.y + R.height))
                           : (ys_[i] < R.y)            ? (R.y - ys_[i])
                                                        : 0.0;
            const double vFloor = vy * vy;
            if (vFloor >= bestCost) {
                return;  // lower bound already loses
            }
            RowUndo undo;
            const double delta = place(R, i, undo);
            if (!std::isfinite(delta)) {
                return;
            }
            // Score non-destructively: the row is restored either way, and the
            // winner is re-placed once at the end. One extra place() per cell is
            // far cheaper than a snapshot per candidate.
            const double total = delta + vFloor;
            if (total < bestCost) {
                bestCost = total;
                bestRow = ri;
            }
            rollback(R, undo);
        };

        tryRow(home);
        for (std::size_t step = 1; step < rows_.size(); ++step) {
            if (params.maxRowDistance > 0 && step > params.maxRowDistance) {
                break;
            }
            // Once the cheapest possible remaining row already costs more than
            // the incumbent, stop. This is the paper's bounding rule and is the
            // difference between scanning 890 rows per cell and scanning a few.
            const double bound = [&] {
                const double a = (home + step < rows_.size())
                                     ? std::fabs(rows_[home + step].y - rows_[home].y)
                                     : std::numeric_limits<double>::max();
                const double b = (step <= home) ? std::fabs(rows_[home - step].y - rows_[home].y)
                                                : std::numeric_limits<double>::max();
                return std::min(a, b);
            }();
            if (bound * bound > bestCost) {
                break;
            }
            tryRow(home + step);
            if (step <= home) {
                tryRow(home - step);
            }
        }

        if (bestRow == kNoRow) {
            ++res.unplaced;
            continue;
        }
        // Commit: place the cell in the winning row for real.
        RowUndo commitUndo;
        place(rows_[bestRow], i, commitUndo);
        ++placed;

        if (!params.plotDir.empty() && params.frameEvery > 0 && placed % params.frameEvery == 0) {
            char name[64];
            std::snprintf(name, sizeof(name), "/legalize_%06zu.svg", placed);
            writeFrame(params.plotDir + name, "legalizing", placed, mov_.size());
        }
    }

    // Materialise cell positions from the cluster structure. Doing this once at
    // the end, rather than inside place(), keeps the inner loop free of any
    // per-member work and is what lets a rejected candidate cost nothing.
    for (const RowTrack &R : rows_) {
        for (const Cluster &c : R.clusters) {
            double off = 0.0;
            for (const std::size_t m : c.members) {
                xs_[m] = c.x + off;
                ys_[m] = R.y;
                off += w_[m];
            }
        }
    }

    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const double dx = xs_[i] - x0_[i];
        const double dy = ys_[i] - y0_[i];
        res.totalSquaredDisplacement += dx * dx + dy * dy;
        res.maxDisplacement = std::max(res.maxDisplacement, std::hypot(dx, dy));
        db_.setCellPosition(mov_[i], xs_[i], ys_[i]);
    }
    res.hpwlAfter = hpwlOf(xs_, ys_);
    res.unplaced = mov_.size() - placed;
    if (!params.plotDir.empty()) {
        char name[64];
        std::snprintf(name, sizeof(name), "/legalize_%06zu.svg", placed);
        writeFrame(params.plotDir + name, "legal placement", placed, mov_.size());
    }
    selfCheck(res);
    res.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    return res;
}

// ---------------------------------------------------------------------------

AbacusLegalizer::AbacusLegalizer(PlacementDB &db) : pImpl(std::make_unique<Impl>(db)) {}

AbacusLegalizer::~AbacusLegalizer() = default;

LegalizeResult AbacusLegalizer::legalize(const LegalizeParams &params) {
    return pImpl->run(params);
}

}  // namespace ktplace
