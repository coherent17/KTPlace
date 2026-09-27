/**
 * @file kt_fastdp.cc
 * @brief Fast detailed placement. See kt_fastdp.h for the technique summary.
 */

#include "detailPlacer/kt_fastdp.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>

#include "datamodel/kt_graph.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"
#include "visualization/kt_plotter.h"

namespace ktplace {

namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

/// One placeable span, mirroring the legalizer's subrows so a cell can only ever
/// be exchanged with another cell in the same span.
struct Span {
    double ylo = 0.0, yhi = 0.0;
    double xlo = 0.0, xhi = 0.0;
    double site = 1.0;
    /// Row pitch, taken as the row height. Vertical swaps compare row distances
    /// against this; comparing against the site width (1 on adaptec1, whose row
    /// pitch is 12) made every pair look non-adjacent and the technique never ran.
    double pitch = 1.0;
    std::size_t row = 0;             ///< index of the row this span belongs to
    std::vector<std::size_t> cells;  ///< ascending x

    [[nodiscard]] bool contains(double px, double pw) const {
        return px >= xlo - 1e-6 && px + pw <= xhi + 1e-6;
    }
};

/// A pin on a net. Fixed pins carry their absolute x and never move.
struct NetPin {
    std::size_t slot;   ///< kNoSlot for a fixed pin
    double offX = 0.0;  ///< offset from the cell's origin
    double absX = 0.0;  ///< used when slot == kNoSlot
};

struct FixedBox {
    double y0, y1, x0, x1;
};

}  // namespace

// ---------------------------------------------------------------------------

class FastDetailedPlacer::Impl {
public:
    explicit Impl(PlacementDB &db) : db_(db), graph_(db.getGraph()) {}

    DetailPlaceResult place(const DetailPlaceParams &params);

private:
    void buildSpans();
    void buildNetlist();
    [[nodiscard]] double hpwl() const;
    /// Exact HPWL change from moving cell c to nx, given a scratch copy of the
    /// affected positions.
    [[nodiscard]] double deltaMove(std::size_t c, double nx) const;
    /// Exact HPWL change from exchanging two cells.
    [[nodiscard]] double deltaSwap(std::size_t a, double na, std::size_t b, double nb) const;
    /// Can c sit at nx without overlapping a neighbour, a macro, or leaving its
    /// span? The span bounds and the site grid are checked; the neighbour check
    /// is done against the cells sorted by x in the span.
    [[nodiscard]] bool canPlace(std::size_t c, double nx) const;
    /// The x that minimises this cell's own net span lengths: the median of the
    /// interval its nets allow.
    [[nodiscard]] double medianX(std::size_t c) const;
    std::size_t globalSwap();
    std::size_t verticalSwap();
    std::size_t localReorder();
    std::size_t singleSegmentCluster();
    void selfCheck(DetailPlaceResult &res) const;
    void writeFrame(const std::string &path, const char *note) const;
    void commit(std::size_t c, double nx);
    /// Re-sort a span's cells by x. The neighbour check in canPlace() binary
    /// searches that order, so it has to be restored after every move.
    void resort(std::size_t s);
    /// Spans in the rows immediately above and below each row, built once.
    std::vector<std::vector<std::size_t>> rowsNear_;
    void removeFromSpan(std::size_t c);
    /// Could cell c legally sit at nx in span s, ignoring the cells in `ignore`?
    /// This is checked *before* any mutation so an exchange never has to be
    /// rolled back: the earlier mutate-then-validate version left 11 overlapping
    /// pairs and 67 cells unassigned when a restore did not land exactly.
    [[nodiscard]] bool fitsIgnoring(std::size_t c, double nx, std::size_t s,
                                    const std::size_t *ignore, std::size_t nIgnore) const;
    /// Find the span cell c currently sits in, and re-check bounds, site grid
    /// and macros for it. False if it is not legally placeable anywhere.
    bool locate(std::size_t c);
    std::size_t localWindow_ = 8;

    PlacementDB &db_;
    Graph &graph_;
    std::vector<std::size_t> mov_;     ///< graph vertex per movable slot
    std::vector<double> w_, h_;        ///< per movable slot
    std::vector<double> x_, y_;        ///< current position
    std::vector<std::size_t> spanOf_;  ///< span index per movable slot
    std::vector<Span> spans_;
    std::vector<std::vector<NetPin>> netPins_;
    /// For each slot, the nets it appears on.
    std::vector<std::vector<std::size_t>> cellNets_;
    std::vector<FixedBox> fixed_;
    BBox die_ = {0.0, 0.0, 0.0, 0.0};
};

void FastDetailedPlacer::Impl::buildSpans() {
    spans_.clear();
    std::vector<PlacementDB::RowInfo> rows = db_.getRows();
    std::sort(rows.begin(), rows.end(),
              [](const PlacementDB::RowInfo &a, const PlacementDB::RowInfo &b) {
                  return a.coordinate < b.coordinate;
              });
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const PlacementDB::RowInfo &ri = rows[r];
        const double site = ri.pitch() > 0.0 ? ri.pitch() : 1.0;
        for (const PlacementDB::SubrowInfo &si : ri.subrows) {
            Span sp;
            sp.row = r;
            sp.ylo = ri.coordinate;
            sp.yhi = ri.coordinate + ri.height;
            sp.xlo = si.xlo();
            sp.xhi = si.xhi(site);
            sp.site = site;
            sp.pitch = ri.height > 0.0 ? ri.height : 1.0;
            if (sp.xhi > sp.xlo) {
                spans_.push_back(std::move(sp));
            }
        }
    }
    spanOf_.assign(mov_.size(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        for (std::size_t s = 0; s < spans_.size(); ++s) {
            const Span &sp = spans_[s];
            if (y_[i] >= sp.ylo - 1e-6 && y_[i] < sp.yhi - 1e-6 && sp.contains(x_[i], w_[i])) {
                spanOf_[i] = s;
                spans_[s].cells.push_back(i);
                break;
            }
        }
    }
    for (Span &sp : spans_) {
        std::sort(sp.cells.begin(), sp.cells.end(), [&](std::size_t a, std::size_t b) {
            return x_[a] < x_[b];
        });
    }

    // Index of the spans in the rows immediately above and below each row, so a
    // vertical swap does not have to walk the whole design per cell.
    rowsNear_.assign(rows.size(), {});
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        const std::size_t r = spans_[s].row;
        if (r > 0) {
            for (std::size_t t = 0; t < spans_.size(); ++t) {
                if (spans_[t].row + 1 == r) {
                    rowsNear_[r].push_back(t);
                }
            }
        }
        if (r + 1 < rows.size()) {
            for (std::size_t t = 0; t < spans_.size(); ++t) {
                if (spans_[t].row == r + 1) {
                    rowsNear_[r].push_back(t);
                }
            }
        }
    }
}

bool FastDetailedPlacer::Impl::fitsIgnoring(std::size_t c, double nx, std::size_t s,
                                            const std::size_t *ignore, std::size_t nIgnore) const {
    const Span &sp = spans_[s];
    if (!sp.contains(nx, w_[c])) {
        return false;
    }
    if (sp.site > 0.0) {
        const double q = nx / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(nx))) {
            return false;
        }
    }
    const std::vector<std::size_t> &cells = sp.cells;
    const auto it = std::lower_bound(cells.begin(), cells.end(), nx, [&](std::size_t a, double v) {
        return x_[a] < v;
    });
    for (auto k = it; k != cells.end() && x_[*k] < nx + w_[c] - 1e-6; ++k) {
        if (std::find(ignore, ignore + nIgnore, *k) == ignore + nIgnore) {
            return false;
        }
    }
    if (it != cells.begin()) {
        const std::size_t prev = *std::prev(it);
        if (x_[prev] + w_[prev] > nx + 1e-6 &&
            std::find(ignore, ignore + nIgnore, prev) == ignore + nIgnore) {
            return false;
        }
    }
    for (const FixedBox &f : fixed_) {
        if (nx + w_[c] > f.x0 + 1e-6 && nx < f.x1 - 1e-6 && y_[c] + h_[c] > f.y0 + 1e-6 &&
            y_[c] < f.y1 - 1e-6) {
            return false;
        }
    }
    return true;
}

void FastDetailedPlacer::Impl::removeFromSpan(std::size_t c) {
    const std::size_t s = spanOf_[c];
    if (s == kNoSlot) {
        return;
    }
    std::vector<std::size_t> &cells = spans_[s].cells;
    cells.erase(std::remove(cells.begin(), cells.end(), c), cells.end());
    spanOf_[c] = kNoSlot;
}

void FastDetailedPlacer::Impl::buildNetlist() {
    std::vector<std::size_t> slotOf(graph_.getNumVertices(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        slotOf[mov_[i]] = i;
    }
    netPins_.assign(graph_.getNumVertices(), {});
    cellNets_.assign(mov_.size(), {});
    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Net) {
            continue;
        }
        for (const std::size_t eid : vert.inEdges) {
            const Edge &e = graph_.getEdge(eid);
            const std::size_t s = slotOf[e.source];
            if (s == kNoSlot) {
                const Vertex &c = graph_.getVertex(e.source);
                netPins_[v].push_back(NetPin{kNoSlot, e.offsetX, c.x + e.offsetX});
            } else {
                netPins_[v].push_back(NetPin{s, e.offsetX, 0.0});
                cellNets_[s].push_back(v);
            }
        }
    }
}

double FastDetailedPlacer::Impl::hpwl() const {
    double total = 0.0;
    for (std::size_t v = 0; v < netPins_.size(); ++v) {
        const std::vector<NetPin> &pins = netPins_[v];
        if (pins.size() < 2) {
            continue;
        }
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max();
        double by = -std::numeric_limits<double>::max();
        bool anyMovable = false;
        for (const NetPin &p : pins) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            ax = std::min(ax, px);
            bx = std::max(bx, px);
            if (p.slot != kNoSlot) {
                anyMovable = true;
                ay = std::min(ay, y_[p.slot]);
                by = std::max(by, y_[p.slot] + h_[p.slot]);
            }
        }
        // Both axes: the legalizer measures HPWL the same way, so the two stages
        // have to agree or the reported change is meaningless. A net whose pins
        // are all fixed has no y extent to speak of, and leaving the sentinels in
        // place made the total -inf.
        if (!anyMovable) {
            ay = 0.0;
            by = 0.0;
        }
        total += (bx - ax) + (by - ay);
    }
    return total;
}

double FastDetailedPlacer::Impl::deltaMove(std::size_t c, double nx) const {
    const double old = x_[c];
    if (std::fabs(old - nx) < 1e-12) {
        return 0.0;
    }
    // Only the nets this cell is on can change, and the new x of this one cell
    // is the only unknown, so each affected net is re-bounded directly.
    double delta = 0.0;
    for (const std::size_t n : cellNets_[c]) {
        const std::vector<NetPin> &pins = netPins_[n];
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : pins) {
            double px;
            if (p.slot == kNoSlot) {
                px = p.absX;
            } else if (p.slot == c) {
                px = nx + p.offX;
            } else {
                px = x_[p.slot] + p.offX;
            }
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta += (bx - ax);
    }
    // Subtract the old contribution of the same nets.
    for (const std::size_t n : cellNets_[c]) {
        const std::vector<NetPin> &pins = netPins_[n];
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : pins) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta -= (bx - ax);
    }
    return delta;
}

double FastDetailedPlacer::Impl::deltaSwap(std::size_t a, double na, std::size_t b,
                                           double nb) const {
    const double oa = x_[a];
    const double ob = x_[b];
    double delta = 0.0;
    // Union of the nets of both cells, deduplicated.
    std::vector<std::size_t> nets = cellNets_[a];
    nets.insert(nets.end(), cellNets_[b].begin(), cellNets_[b].end());
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    for (const std::size_t n : nets) {
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : netPins_[n]) {
            double px;
            if (p.slot == kNoSlot) {
                px = p.absX;
            } else if (p.slot == a) {
                px = na + p.offX;
            } else if (p.slot == b) {
                px = nb + p.offX;
            } else {
                px = x_[p.slot] + p.offX;
            }
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta += (bx - ax);
        ax = std::numeric_limits<double>::max();
        bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : netPins_[n]) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta -= (bx - ax);
    }
    (void)oa;
    (void)ob;
    return delta;
}

double FastDetailedPlacer::Impl::medianX(std::size_t c) const {
    // Collect the interval each net allows this cell to move in, then take the
    // median of the interval endpoints. This is the classic median move: it is
    // the x that minimises the sum of this cell's own net spans.
    std::vector<double> los, his;
    los.reserve(cellNets_[c].size());
    his.reserve(cellNets_[c].size());
    for (const std::size_t n : cellNets_[c]) {
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        double moff = 0.0;
        bool has = false;
        for (const NetPin &p : netPins_[n]) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            if (p.slot == c) {
                moff = p.offX;
                has = true;
            }
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        if (!has) {
            continue;
        }
        // Keep the pin inside the other pins' span: the cell's left edge may run
        // from (min - off) to (max - off).
        los.push_back(ax - moff);
        his.push_back(bx - moff);
    }
    if (los.empty()) {
        return x_[c];
    }
    std::vector<double> mids;
    mids.reserve(los.size());
    for (std::size_t i = 0; i < los.size(); ++i) {
        mids.push_back(0.5 * (los[i] + his[i]));
    }
    std::nth_element(mids.begin(), mids.begin() + static_cast<long>(mids.size() / 2), mids.end());
    return mids[mids.size() / 2];
}

bool FastDetailedPlacer::Impl::canPlace(std::size_t c, double nx) const {
    const std::size_t s = spanOf_[c];
    if (s == kNoSlot) {
        return false;
    }
    const Span &sp = spans_[s];
    if (!sp.contains(nx, w_[c])) {
        return false;
    }
    if (sp.site > 0.0) {
        const double q = nx / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(nx))) {
            return false;
        }
    }
    // Neighbours: the span's cells are sorted by x, so only the ones bracketing
    // nx can overlap it. Scanning the whole span would be quadratic overall.
    const std::vector<std::size_t> &cells = sp.cells;
    const auto it = std::lower_bound(cells.begin(), cells.end(), nx, [&](std::size_t a, double v) {
        return x_[a] < v;
    });
    if (it != cells.end() && *it != c && x_[*it] < nx + w_[c] - 1e-6) {
        return false;
    }
    if (it != cells.begin()) {
        const std::size_t prev = *std::prev(it);
        if (prev != c && x_[prev] + w_[prev] > nx + 1e-6) {
            return false;
        }
    }
    // Macros.
    for (const FixedBox &f : fixed_) {
        if (nx + w_[c] > f.x0 + 1e-6 && nx < f.x1 - 1e-6) {
            const double cy0 = y_[c], cy1 = y_[c] + h_[c];
            if (cy1 > f.y0 + 1e-6 && cy0 < f.y1 - 1e-6) {
                return false;
            }
        }
    }
    return true;
}

void FastDetailedPlacer::Impl::commit(std::size_t c, double nx) {
    x_[c] = nx;
}

bool FastDetailedPlacer::Impl::locate(std::size_t c) {
    spanOf_[c] = kNoSlot;
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        const Span &sp = spans_[s];
        if (y_[c] < sp.ylo - 1e-6 || y_[c] >= sp.yhi - 1e-6) {
            continue;
        }
        if (!sp.contains(x_[c], w_[c])) {
            continue;
        }
        const double q = x_[c] / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(x_[c]))) {
            continue;
        }
        bool blocked = false;
        for (const FixedBox &f : fixed_) {
            if (x_[c] + w_[c] > f.x0 + 1e-6 && x_[c] < f.x1 - 1e-6 && y_[c] + h_[c] > f.y0 + 1e-6 &&
                y_[c] < f.y1 - 1e-6) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }
        // Overlap against the span's other cells. Without this a vertical swap
        // could land a cell on top of a third cell: locate() checked bounds, the
        // site grid and the macros, but not its neighbours, and that left 67298
        // overlapping pairs. The span's cells are sorted by x, so only the ones
        // bracketing this position can overlap it.
        const std::vector<std::size_t> &others = spans_[s].cells;
        const auto it =
            std::lower_bound(others.begin(), others.end(), x_[c], [&](std::size_t a, double v) {
                return x_[a] < v;
            });
        if (it != others.end() && x_[*it] < x_[c] + w_[c] - 1e-6) {
            continue;
        }
        if (it != others.begin()) {
            const std::size_t prev = *std::prev(it);
            if (x_[prev] + w_[prev] > x_[c] + 1e-6) {
                continue;
            }
        }
        spanOf_[c] = s;
        spans_[s].cells.push_back(c);
        return true;
    }
    return false;
}

void FastDetailedPlacer::Impl::resort(std::size_t s) {
    std::vector<std::size_t> &cells = spans_[s].cells;
    std::sort(cells.begin(), cells.end(), [&](std::size_t a, std::size_t b) {
        return x_[a] < x_[b];
    });
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    std::size_t moves = 0;
    for (std::size_t c = 0; c < mov_.size(); ++c) {
        const std::size_t s = spanOf_[c];
        if (s == kNoSlot) {
            continue;
        }
        const double want = medianX(c);
        if (!canPlace(c, want)) {
            continue;
        }
        if (deltaMove(c, want) < -1e-9) {
            commit(c, want);
            resort(s);
            ++moves;
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    // Exchange a cell with one in an adjacent row. Only spans in the rows
    // immediately above and below are considered: walking every span in the
    // design for every cell made this quadratic in the design and effectively
    // hung on adaptec1. Within a candidate span only the cells bracketing the
    // partner's x are tried, since a swap that is not x-adjacent cannot pay.
    std::size_t moves = 0;
    std::vector<std::size_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);

    for (const std::size_t c : order) {
        const std::size_t sc = spanOf_[c];
        if (sc == kNoSlot) {
            continue;
        }
        const Span &spc = spans_[sc];
        for (const std::size_t s2 : rowsNear_[spc.row]) {
            if (s2 == sc) {
                continue;
            }
            Span &sp2 = spans_[s2];
            // Nearest cells in x on the other side, in both directions.
            const std::vector<std::size_t> &oc = sp2.cells;
            if (oc.empty()) {
                continue;
            }
            const auto at =
                std::lower_bound(oc.begin(), oc.end(), x_[c], [&](std::size_t a, double v) {
                    return x_[a] < v;
                });
            const std::size_t cands[2] = {
                (at == oc.end()) ? kNoSlot : *at,
                (at == oc.begin()) ? kNoSlot : *std::prev(at),
            };
            for (const std::size_t d : cands) {
                if (d == kNoSlot) {
                    continue;
                }
                // Validate the whole exchange before touching anything.
                const std::size_t ignore[2] = {c, d};
                const double ocx = x_[c], odx = x_[d];
                if (!fitsIgnoring(c, odx, s2, ignore, 2) || !fitsIgnoring(d, ocx, sc, ignore, 2)) {
                    continue;
                }
                if (deltaSwap(c, odx, d, ocx) >= -1e-9) {
                    continue;
                }
                // The two cells also change row, so clear the macro test for the
                // y each one lands at.
                y_[c] = spans_[s2].ylo;
                y_[d] = spans_[sc].ylo;
                if (!fitsIgnoring(c, odx, s2, ignore, 2) || !fitsIgnoring(d, ocx, sc, ignore, 2)) {
                    y_[c] = spans_[sc].ylo;
                    y_[d] = spans_[s2].ylo;
                    continue;
                }
                removeFromSpan(c);
                removeFromSpan(d);
                commit(c, odx);
                commit(d, ocx);
                spanOf_[c] = s2;
                spanOf_[d] = sc;
                spans_[s2].cells.push_back(c);
                spans_[sc].cells.push_back(d);
                resort(s2);
                resort(sc);
                ++moves;
                break;
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::localReorder() {
    // For each window of consecutive cells in a span, find the best left-to-right
    // ordering exactly. With the order fixed and the cells packed from the
    // window's left edge, each ordering is scored by the resulting positions, and
    // a subset dynamic program over 2^k states finds the optimum.
    std::size_t moves = 0;
    const std::size_t k = std::min<std::size_t>(localWindow_, 12);
    if (k < 2) {
        return 0;
    }
    for (Span &sp : spans_) {
        const std::size_t n = sp.cells.size();
        if (n < 2) {
            continue;
        }
        for (std::size_t start = 0; start + 1 < n; start += k) {
            const std::size_t len = std::min(k, n - start);
            if (len < 2) {
                break;
            }
            const std::size_t states = static_cast<std::size_t>(1) << len;
            std::vector<double> cost(states, std::numeric_limits<double>::infinity());
            std::vector<std::size_t> last(states, kNoSlot);
            cost[0] = 0.0;
            const double base = std::floor(x_[sp.cells[start]] / sp.site) * sp.site;
            for (std::size_t mask = 0; mask < states; ++mask) {
                if (!std::isfinite(cost[mask])) {
                    continue;
                }
                // The window's left edge is where the cells already are. Packing
                // from the subrow's left edge instead explores orderings that are
                // all worse than the current one, so no improvement is ever found.
                double cursor = base;
                // Rebuild the cursor for this mask by summing the widths already
                // placed, which is what the packing constraint is.
                for (std::size_t b = 0; b < len; ++b) {
                    if (mask & (static_cast<std::size_t>(1) << b)) {
                        cursor += w_[sp.cells[start + b]];
                    }
                }
                for (std::size_t b = 0; b < len; ++b) {
                    const std::size_t bit = static_cast<std::size_t>(1) << b;
                    if (mask & bit) {
                        continue;
                    }
                    const std::size_t c = sp.cells[start + b];
                    const double nx = cursor;
                    if (nx + w_[c] > sp.xhi + 1e-6) {
                        continue;
                    }
                    const double cand = cost[mask] + deltaMove(c, nx);
                    const std::size_t nm = mask | bit;
                    if (cand < cost[nm]) {
                        cost[nm] = cand;
                        last[nm] = c;
                    }
                }
            }
            const std::size_t all = states - 1;
            if (!std::isfinite(cost[all]) || cost[all] >= -1e-9) {
                continue;  // no ordering improves on the current one
            }
            // Walk the chain back to recover the winning order, then apply it.
            std::vector<std::size_t> order;
            std::size_t mask = all;
            while (mask != 0) {
                const std::size_t c = last[mask];
                if (c == kNoSlot) {
                    order.clear();
                    break;
                }
                order.push_back(c);
                std::size_t bit = 0;
                while (c != sp.cells[start + bit]) {
                    ++bit;
                }
                mask ^= static_cast<std::size_t>(1) << bit;
            }
            if (order.size() != len) {
                continue;
            }
            std::reverse(order.begin(), order.end());
            double cursor = base;
            for (const std::size_t c : order) {
                const double nx = cursor;
                if (deltaMove(c, nx) < -1e-9 || std::fabs(x_[c] - nx) > 1e-9) {
                    commit(c, nx);
                    ++moves;
                }
                cursor = nx + w_[c];
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::singleSegmentCluster() {
    // With the left-to-right order fixed, re-place each span with the
    // legalizer's greedy cluster pass: every cell goes to the site nearest its
    // current x that does not overlap its predecessor. Cells that end up on the
    // same site as a neighbour are pulled apart, which is where the wirelength
    // comes from.
    std::size_t moves = 0;
    for (Span &sp : spans_) {
        const std::size_t n = sp.cells.size();
        if (n < 2) {
            continue;
        }
        std::vector<double> nx(n, 0.0);
        double cursor = sp.xlo;
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = sp.cells[k];
            double want = std::max(x_[c], cursor);
            want = std::round(want / sp.site) * sp.site;
            if (want + w_[c] > sp.xhi + 1e-6) {
                ok = false;
                break;
            }
            nx[k] = want;
            cursor = want + w_[c];
        }
        if (!ok) {
            continue;
        }
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = sp.cells[k];
            if (std::fabs(x_[c] - nx[k]) > 1e-9 && deltaMove(c, nx[k]) < -1e-9) {
                commit(c, nx[k]);
                ++moves;
            }
        }
        // The pass keeps the order, so the span is still sorted by x.
        if (moves != 0) {
            resort(static_cast<std::size_t>(&sp - spans_.data()));
        }
    }
    return moves;
}

void FastDetailedPlacer::Impl::writeFrame(const std::string &path, const char *note) const {
    const std::size_t nv = graph_.getNumVertices();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(graph_.getVertex(v).x);
        fy[v] = static_cast<float>(graph_.getVertex(v).y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(x_[i]);
        fy[mov_[i]] = static_cast<float>(y_[i]);
    }
    writeFrameSvg(path, graph_, fx, fy, die_, 0, 1, hpwl(), 0.0, 0.0, note, nullptr,
                  /*fixedView=*/true);
    // Into the run's animation as well, so detailed placement's contribution --
    // usually the last thing that moves cells -- is in the GIF too.
    PlacementAnimator::instance().record(graph_, fx, fy, die_, 0, 1, hpwl(), hpwl(), 0.0, note);
}

void FastDetailedPlacer::Impl::selfCheck(DetailPlaceResult &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        if (spanOf_[i] == kNoSlot) {
            ++res.offRow;
            continue;
        }
        const Span &sp = spans_[spanOf_[i]];
        if (!sp.contains(x_[i], w_[i])) {
            ++res.offRow;
            continue;
        }
        const double q = x_[i] / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(x_[i]))) {
            ++res.offSite;
        }
    }
    for (const Span &sp : spans_) {
        for (std::size_t k = 1; k < sp.cells.size(); ++k) {
            const std::size_t a = sp.cells[k - 1], b = sp.cells[k];
            if (x_[a] + w_[a] > x_[b] + eps) {
                ++res.overlappingPairs;
            }
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        for (const FixedBox &f : fixed_) {
            if (x_[i] + w_[i] > f.x0 + eps && x_[i] < f.x1 - eps && y_[i] + h_[i] > f.y0 + eps &&
                y_[i] < f.y1 - eps) {
                ++res.overFixed;
                break;
            }
        }
    }
}

DetailPlaceResult FastDetailedPlacer::Impl::place(const DetailPlaceParams &params) {
    DetailPlaceResult res;
    ScopedTimer timer("detail-place");
    localWindow_ = params.localReorderWindow;

    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type == VertexType::Cell && vert.isFixed) {
            fixed_.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
        }
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        mov_.push_back(v);
        w_.push_back(vert.width);
        h_.push_back(vert.height);
        x_.push_back(vert.x);
        y_.push_back(vert.y);
    }
    if (mov_.empty()) {
        return res;
    }
    const BBox d = fixedCellBBox(graph_);
    die_ = BBox{d[0], d[1], d[2], d[3]};
    buildSpans();
    buildNetlist();
    res.hpwlBefore = hpwl();

    if (!params.plotDir.empty()) {
        std::filesystem::create_directories(params.plotDir);
        writeFrame(params.plotDir + "/dp_000.svg", "legalized input");
    }

    double prev = res.hpwlBefore;
    int pass = 0;
    const auto sweep = [&](const char *name, std::size_t limit, std::size_t &counter) {
        for (std::size_t k = 0; k < limit; ++k) {
            const std::size_t before = counter;
            if (name[0] == 'g') {
                counter = globalSwap();
            } else if (name[0] == 'v') {
                counter = verticalSwap();
            } else if (name[0] == 'l') {
                counter = localReorder();
            } else {
                counter = singleSegmentCluster();
            }
            const double now = hpwl();
            char note[128];
            std::snprintf(note, sizeof(note), "%s pass %zu (hpwl %.6g)", name, k, now);
            if (!params.plotDir.empty()) {
                char path[64];
                std::snprintf(path, sizeof(path), "/dp_%03d_%s_%zu.svg", ++pass, name, k);
                writeFrame(params.plotDir + path, note);
            }
            if (counter == 0) {
                break;
            }
            if (prev > 0.0 && (prev - now) / prev < params.minImprovement) {
                break;  // converged
            }
            prev = now;
            (void)before;
        }
    };

    sweep("global", params.globalSwapPasses, res.globalSwaps);
    sweep("vertical", params.verticalSwapPasses, res.verticalSwaps);
    sweep("reorder", params.localReorderPasses, res.reorderMoves);
    sweep("cluster", params.clusterPasses, res.clusterMoves);

    res.hpwlAfter = hpwl();
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        db_.setCellPosition(mov_[i], x_[i], y_[i]);
    }
    selfCheck(res);
    res.seconds = timer.elapsedSeconds();
    ktlog.echo(
        "FastDP: {:.3f}s, HPWL {:.6e} -> {:.6e} ({:+.2f}%), swaps {} global / {} vertical, "
        "reorder {}, cluster {}",
        res.seconds, res.hpwlBefore, res.hpwlAfter,
        (res.hpwlBefore > 0.0) ? 100.0 * (res.hpwlAfter - res.hpwlBefore) / res.hpwlBefore : 0.0,
        res.globalSwaps, res.verticalSwaps, res.reorderMoves, res.clusterMoves);
    return res;
}

// ---------------------------------------------------------------------------

FastDetailedPlacer::FastDetailedPlacer(PlacementDB &db) : pImpl(std::make_unique<Impl>(db)) {}

FastDetailedPlacer::~FastDetailedPlacer() = default;

DetailPlaceResult FastDetailedPlacer::place(const DetailPlaceParams &params) {
    return pImpl->place(params);
}

}  // namespace ktplace
