// @file abacus_legalizer.cc
// Abacus legalization, DP-over-clusters form. See abacus_legalizer.h.


#include "legalizer/abacus/abacus_legalizer.h"

#include "legalizer/abacus/abacus_design.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>

namespace ktplace {

namespace {

/// Axis-aligned box of a fixed cell (a macro or a pin), used to carve subrows.
struct FixedBox {
    double y0, y1, x0, x1;
};

/// Cost of a candidate that does not fit; no real cost reaches this.
constexpr double kInfeasible = std::numeric_limits<double>::infinity();

struct Span {
    double n = 0.0;
    double w = 0.0;
    double a = 0.0;
    double b = 0.0;

    [[nodiscard]] double costAt(double pos) const {
        return n * pos * pos + 2.0 * a * pos + b;
    }
    [[nodiscard]] double ideal() const {
        return n > 0.0 ? -a / n : 0.0;
    }
    void append(const Span &right) {
        a += right.a + right.n * w;
        b += right.b + 2.0 * w * right.a + right.n * w * w;
        n += right.n;
        w += right.w;
    }
};

struct Cluster {
    Span span;
    double x = 0.0;
    std::vector<std::size_t> members;
};

struct Trial {
    double delta = kInfeasible;
    std::size_t first = 0;
    double x = 0.0;
    Span span;
};

/// One contiguous run of placeable sites within a row.
///
/// A row is not one span of x. Macros cut it into subrows of very different
/// widths, so placing into a row means choosing a subrow first, and a subrow
/// narrower than a cell cannot hold it at all. Modelling a row as a single span
/// (as an earlier version did) made the placer slide cells along x straight
/// across a blockage, which is what destroyed the wirelength: a cell that only
/// needed to change rows was instead dragged to the far end of its own row.
struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
    /// The fence this piece belongs to, or kNoRegion for open space. Only a cell
    /// of the same region may be placed here.
    int region = abacus::kNoRegion;
    /// Clusters assigned here, in increasing left edge.
    std::vector<Cluster> clusters;
    /// Committed right edge of the last cluster, i.e. the used extent.
    double used = 0.0;

    [[nodiscard]] double width() const {
        return xhi - xlo;
    }
    [[nodiscard]] double free() const {
        return xhi - used;
    }
};

/// One placement row: a y band plus the subrows inside it.
struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double siteWidth = 1.0;
    std::vector<Subrow> subrows;

    /// Total placeable width left in the row.
    [[nodiscard]] double capacity() const {
        double t = 0.0;
        for (const Subrow &sr : subrows) {
            t += sr.free();
        }
        return t;
    }
};

}  // namespace

// ---------------------------------------------------------------------------

class AbacusLegalizer::Impl {
public:
    explicit Impl(const abacus::Design &d) : design(d) {}

    LegalizeResult run(const AbacusOptions &params);

private:
    void buildRows(bool mirrored);
    struct Pass {
        std::vector<double> xs;
        std::vector<double> ys;
        std::size_t placed = 0;
        double movement = 0.0;
    };
    [[nodiscard]] Pass legalizePass(const AbacusOptions &params, bool mirrored);
    [[nodiscard]] Trial trial(const Subrow &sr, double grid, std::size_t i) const;
    void commit(Subrow &sr, std::size_t i, const Trial &t);
    [[nodiscard]] double hpwlOf(const std::vector<double> &x, const std::vector<double> &y) const;
    /// Offer a snapshot to the sink. A no-op when the stage has no sink, so the
    /// callers below do not each have to ask whether drawing is on.
    void writeFrame(const std::string &note, std::size_t step, std::size_t total) const;
    void selfCheck(LegalizeResult &res) const;

    static constexpr std::size_t kNoRow = std::numeric_limits<std::size_t>::max();

    const abacus::Design &design;
    std::vector<RowTrack> rows_;
    std::vector<std::size_t> mov_;  ///< graph vertex id per movable slot
    std::vector<double> w_, h_;     ///< per movable slot
    std::vector<double> x0_, y0_;   ///< pre-legalization position (the target)
    std::vector<double> tx_;
    std::vector<double> xs_, ys_;  ///< live position
    std::vector<int> region_;      ///< fence of each movable slot, or kNoRegion
    std::unordered_map<std::size_t, std::size_t> slotOf_;
    std::vector<FixedBox> fixed_;
    abacus::DieBox die_ = {0.0, 0.0, 0.0, 0.0};
    double minSubrowSites_ = 1.0;

public:
    /// What legalize() reads to build the placement it hands back. The pass never
    /// writes to the design; these carry its answer out instead.
    std::vector<std::size_t> movableIds;
    std::vector<double> finalXs;
    std::vector<double> finalYs;
};

void AbacusLegalizer::Impl::buildRows(bool mirrored) {
    rows_.clear();
    for (const abacus::RowInfo &ri : design.rows) {
        RowTrack r;
        r.y = ri.coordinate;
        r.height = ri.height;
        r.siteWidth = ri.pitch();
        if (!(r.siteWidth > 0.0)) {
            r.siteWidth = 1.0;
        }
        // Each .scl subrow becomes a Subrow, trimmed by any fixed cell that
        // crosses the row's band, so a macro crossing a subrow shortens it
        // instead of being ignored.
        for (const abacus::Subrow &si : ri.subrows) {
            if (!(si.xhi > si.xlo)) {
                continue;
            }
            Subrow sr;
            sr.xlo = si.xlo;
            sr.xhi = si.xhi;
            sr.region = si.region;
            sr.used = sr.xlo;
            r.subrows.push_back(sr);
        }
        if (r.subrows.empty()) {
            continue;
        }
        std::sort(r.subrows.begin(), r.subrows.end(), [](const Subrow &a, const Subrow &b) {
            return a.xlo < b.xlo;
        });
        rows_.push_back(r);
    }
    std::sort(rows_.begin(), rows_.end(), [](const RowTrack &a, const RowTrack &b) {
        return a.y < b.y;
    });

    // Trim each subrow against the fixed cells crossing its row band. A macro
    // that lands inside a .scl subrow splits it into two placeable subrows, so
    // the trimmed pieces are what the search actually chooses between.
    for (RowTrack &r : rows_) {
        std::vector<Subrow> pieces;
        for (const Subrow &sr : r.subrows) {
            std::vector<std::pair<double, double>> hit;
            for (const FixedBox &f : fixed_) {
                if (f.y1 <= r.y + 1e-9 || f.y0 >= r.y + r.height - 1e-9) {
                    continue;
                }
                const double a = std::max(f.x0, sr.xlo);
                const double b = std::min(f.x1, sr.xhi);
                if (b > a) {
                    hit.emplace_back(a, b);
                }
            }
            std::sort(hit.begin(), hit.end());
            double cur = sr.xlo;
            for (const auto &iv : hit) {
                if (iv.first > cur) {
                    Subrow piece = sr;
                    piece.xlo = cur;
                    piece.xhi = iv.first;
                    piece.used = cur;
                    pieces.push_back(piece);
                }
                cur = std::max(cur, iv.second);
            }
            if (cur < sr.xhi) {
                Subrow piece = sr;
                piece.xlo = cur;
                piece.used = cur;
                pieces.push_back(piece);
            }
        }
        r.subrows = std::move(pieces);
    }
    if (mirrored) {
        for (RowTrack &r : rows_) {
            for (Subrow &sr : r.subrows) {
                const double lo = -sr.xhi;
                sr.xhi = -sr.xlo;
                sr.xlo = lo;
                sr.used = lo;
            }
            std::reverse(r.subrows.begin(), r.subrows.end());
        }
    }
    for (RowTrack &r : rows_) {
        // Subrows too narrow to be worth searching are dropped: a sliver a cell
        // cannot fit into only makes the search slower and can never be chosen.
        std::vector<Subrow> kept;
        for (Subrow &piece : r.subrows) {
            if (piece.width() >= r.siteWidth * minSubrowSites_) {
                kept.push_back(std::move(piece));
            }
        }
        r.subrows = std::move(kept);
    }
}

Trial AbacusLegalizer::Impl::trial(const Subrow &sr, double grid, std::size_t i) const {
    const auto position = [&](const Span &span) {
        double x = std::clamp(span.ideal(), sr.xlo, std::max(sr.xlo, sr.xhi - span.w));
        if (grid > 0.0) {
            x = std::round(x / grid) * grid;
            if (x + span.w > sr.xhi + 1e-9) {
                x -= grid;
            }
            if (x < sr.xlo - 1e-9) {
                x += grid;
            }
        }
        return x;
    };

    Span span{1.0, w_[i], -tx_[i], tx_[i] * tx_[i]};
    std::size_t first = sr.clusters.size();
    double before = 0.0;
    double x = position(span);
    while (first > 0 && sr.clusters[first - 1].x + sr.clusters[first - 1].span.w > x + 1e-9) {
        const Cluster &prev = sr.clusters[first - 1];
        before += prev.span.costAt(prev.x);
        Span merged = prev.span;
        merged.append(span);
        span = merged;
        --first;
        x = position(span);
    }
    if (x < sr.xlo - 1e-9 || x + span.w > sr.xhi + 1e-9) {
        return {};
    }
    return {span.costAt(x) - before, first, x, span};
}

void AbacusLegalizer::Impl::commit(Subrow &sr, std::size_t i, const Trial &t) {
    Cluster merged;
    merged.span = t.span;
    merged.x = t.x;
    for (std::size_t k = t.first; k < sr.clusters.size(); ++k) {
        std::vector<std::size_t> &m = sr.clusters[k].members;
        merged.members.insert(merged.members.end(), m.begin(), m.end());
    }
    merged.members.push_back(i);
    sr.clusters.erase(sr.clusters.begin() + static_cast<long>(t.first), sr.clusters.end());
    sr.clusters.push_back(std::move(merged));
    sr.used = t.x + t.span.w;
}

double AbacusLegalizer::Impl::hpwlOf(const std::vector<double> &x,
                                     const std::vector<double> &y) const {
    double total = 0.0;
    for (const abacus::Net &net : design.nets) {
        if (net.pins.empty()) {
            continue;
        }
        double ax = std::numeric_limits<double>::max(), bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max(), by = -std::numeric_limits<double>::max();
        for (const std::size_t pinId : net.pins) {
            const abacus::Pin &pin = design.pins[pinId];
            const auto it = slotOf_.find(pin.cellId);
            double cx = 0.0, cy = 0.0;
            if (it != slotOf_.end()) {
                cx = x[it->second] + pin.offsetX;
                cy = y[it->second] + pin.offsetY;
            } else {
                const abacus::Cell &c = design.cells[pin.cellId];
                cx = c.x + pin.offsetX;
                cy = c.y + pin.offsetY;
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

void AbacusLegalizer::Impl::writeFrame(const std::string &note, std::size_t step,
                                       std::size_t total) const {
    const std::size_t nv = design.cells.size();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(design.cells[v].x);
        fy[v] = static_cast<float>(design.cells[v].y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(xs_[i]);
        fy[mov_[i]] = static_cast<float>(ys_[i]);
    }
    // The sink draws; Abacus only says when.
    if (design.onFrame) {
        abacus::FrameInfo info;
        info.note = note;
        info.step = step;
        info.numSteps = total;
        info.hpwl = hpwlOf(xs_, ys_);
        info.hpwlInitial = hpwlOf(xs_, ys_);
        info.mandatory = true;
        design.onFrame(fx, fy, info);
    }
}

void AbacusLegalizer::Impl::selfCheck(LegalizeResult &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.commitFailures = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    res.outOfRows = 0;
    // Row of each cell, read off the cluster structure rather than by scanning
    // every row for every cell.
    std::vector<std::size_t> rowOf(mov_.size(), kNoRow);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const auto it = std::lower_bound(rows_.begin(), rows_.end(), ys_[i] - eps,
                                         [](const RowTrack &r, double y) {
                                             return r.y < y;
                                         });
        if (it != rows_.end() && std::fabs(it->y - ys_[i]) <= eps) {
            rowOf[i] = static_cast<std::size_t>(it - rows_.begin());
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const RowTrack *inRow = (rowOf[i] == kNoRow) ? nullptr : &rows_[rowOf[i]];
        if (inRow == nullptr) {
            ++res.offRow;
            ++res.outOfRows;
            continue;
        }

        const double q = xs_[i] / inRow->siteWidth;
        const double off = std::fabs(q - std::round(q)) * inRow->siteWidth;
        if (off > 1e-6 * std::max(1.0, std::fabs(xs_[i]))) {
            ++res.offSite;
        }
        // Inside some subrow of this row, with the cell on a site?
        bool inside = false;
        for (const Subrow &sr : inRow->subrows) {
            if (xs_[i] >= sr.xlo - eps && xs_[i] + w_[i] <= sr.xhi + eps) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            ++res.outOfRows;
        }
    }

    // Overlap: sort every movable cell by (row, x) and test only adjacent pairs.
    // Comparing each row's cells pairwise is quadratic and was both slow and,
    // before the early-stop fix, a source of phantom overlaps.
    std::vector<std::uint64_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
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
    const std::vector<FixedBox> &fixed = fixed_;
    std::vector<std::size_t> byX(mov_.size());
    std::iota(byX.begin(), byX.end(), 0u);
    std::sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        return xs_[a] < xs_[b];
    });
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

LegalizeResult AbacusLegalizer::Impl::run(const AbacusOptions &params) {
    LegalizeResult res;
    minSubrowSites_ = static_cast<double>(params.minSubrowSites);
    // ScopedTimer both times the phase and records it into the shared registry,
    // so legalization appears in the flow's Timings table next to load, place
    // and write instead of reporting a private duration.
    ScopedTimer timer("legalize");


    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        const abacus::Cell &cell = design.cells[v];
        if (cell.isFixed || cell.isTerminal) {
            continue;
        }
        const std::size_t slot = mov_.size();
        mov_.push_back(v);
        movableIds.push_back(v);
        slotOf_[v] = slot;
        w_.push_back(cell.width);
        h_.push_back(cell.height);
        // A region id the design does not have is treated as unfenced rather than
        // as a fence no piece can ever match.
        region_.push_back(cell.region >= 0 &&
                                  static_cast<std::size_t>(cell.region) < design.fences.numRegions()
                              ? cell.region
                              : abacus::kNoRegion);
        x0_.push_back(cell.x);
        y0_.push_back(cell.y);
    }
    xs_ = x0_;
    ys_ = y0_;
    res.cellsPlaced = mov_.size();
    if (mov_.empty()) {
        return res;
    }
    res.hpwlBefore = hpwlOf(xs_, ys_);

    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        const abacus::Cell &vert = design.cells[v];
        if (!vert.isFixed) {
            continue;
        }
        fixed_.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
    }

    buildRows(false);
    if (rows_.empty()) {
        res.unplaced = mov_.size();
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }
    die_ = design.die;

    if (design.onFrame) {
        writeFrame("input placement", 0, mov_.size());
    }

    Pass forward = legalizePass(params, false);
    Pass backward = legalizePass(params, true);
    const bool useBackward =
        backward.placed > forward.placed ||
        (backward.placed == forward.placed && backward.movement < forward.movement);
    ktlog.trace("  abacus: movement {:.6e} increasing x, {:.6e} decreasing x; using {}",
                forward.movement, backward.movement, useBackward ? "decreasing" : "increasing");
    Pass &chosen = useBackward ? backward : forward;
    xs_ = std::move(chosen.xs);
    ys_ = std::move(chosen.ys);
    buildRows(false);

    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const double dx = xs_[i] - x0_[i];
        const double dy = ys_[i] - y0_[i];
        res.totalSquaredDisplacement += dx * dx + dy * dy;
        res.maxDisplacement = std::max(res.maxDisplacement, std::hypot(dx, dy));
    }
    res.hpwlAfter = hpwlOf(xs_, ys_);

    finalXs.assign(design.cells.size(), 0.0);
    finalYs.assign(design.cells.size(), 0.0);
    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        finalXs[v] = design.cells[v].x;
        finalYs[v] = design.cells[v].y;
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        finalXs[mov_[i]] = xs_[i];
        finalYs[mov_[i]] = ys_[i];
    }
    res.unplaced = mov_.size() - chosen.placed;
    if (design.onFrame) {
        writeFrame("legal placement", chosen.placed, mov_.size());
    }
    selfCheck(res);
    res.seconds = timer.elapsedSeconds();
    ktlog.echo(
        "Abacus: {} cells in {:.3f}s, HPWL {:.6e} -> {:.6e}, squared displacement {:.6e} "
        "(max {:.1f}), {} unplaced",
        res.cellsPlaced, res.seconds, res.hpwlBefore, res.hpwlAfter, res.totalSquaredDisplacement,
        res.maxDisplacement, res.unplaced);
    return res;
}

AbacusLegalizer::Impl::Pass AbacusLegalizer::Impl::legalizePass(const AbacusOptions &params,
                                                                bool mirrored) {
    buildRows(mirrored);
    tx_.resize(mov_.size());
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        tx_[i] = mirrored ? -(x0_[i] + w_[i]) : x0_[i];
    }
    std::vector<std::size_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return tx_[a] < tx_[b];
    });

    Pass pass;
    pass.xs = x0_;
    pass.ys = y0_;
    for (const std::size_t i : order) {
        const auto rowGap = [&](const RowTrack &R) {
            return (y0_[i] < R.y)              ? (R.y - y0_[i])
                   : (y0_[i] > R.y + R.height) ? (y0_[i] - (R.y + R.height))
                                               : 0.0;
        };
        std::size_t home = 0;
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t r = 0; r < rows_.size(); ++r) {
            if (rowGap(rows_[r]) < nearest) {
                nearest = rowGap(rows_[r]);
                home = r;
            }
        }

        std::size_t bestRow = kNoRow;
        std::size_t bestSub = 0;
        double bestCost = std::numeric_limits<double>::max();
        Trial best;
        const auto tryRow = [&](std::size_t ri) {
            if (ri >= rows_.size()) {
                return;
            }
            const RowTrack &R = rows_[ri];
            const double dy = rowGap(R);
            for (std::size_t si = 0; si < R.subrows.size(); ++si) {
                const Subrow &sr = R.subrows[si];
                if (h_[i] > R.height + 1e-9 || sr.region != region_[i] ||
                    w_[i] > sr.width() + 1e-9) {
                    continue;
                }
                const double dx = (tx_[i] < sr.xlo)           ? (sr.xlo - tx_[i])
                                  : (tx_[i] > sr.xhi - w_[i]) ? (tx_[i] - (sr.xhi - w_[i]))
                                                              : 0.0;
                if (dx * dx + dy * dy >= bestCost) {
                    continue;
                }
                const Trial t = trial(sr, R.siteWidth, i);
                if (!std::isfinite(t.delta)) {
                    continue;
                }
                const double moved = t.x + t.span.w - w_[i] - tx_[i];
                const double cost = moved * moved + dy * dy;
                if (cost < bestCost) {
                    bestCost = cost;
                    bestRow = ri;
                    bestSub = si;
                    best = t;
                }
            }
        };

        tryRow(home);
        for (std::size_t step = 1; step < rows_.size(); ++step) {
            if (params.maxRowDistance > 0 && step > params.maxRowDistance) {
                break;
            }
            const double up = home + step < rows_.size() ? rows_[home + step].y - rows_[home].y
                                                         : std::numeric_limits<double>::max();
            const double down = step <= home ? rows_[home].y - rows_[home - step].y
                                             : std::numeric_limits<double>::max();
            if (std::min(up, down) * std::min(up, down) > bestCost) {
                break;
            }
            tryRow(home + step);
            if (step <= home) {
                tryRow(home - step);
            }
        }
        if (bestRow == kNoRow) {
            for (std::size_t ri = 0; ri < rows_.size(); ++ri) {
                tryRow(ri);
            }
        }
        if (bestRow == kNoRow) {
            continue;
        }
        commit(rows_[bestRow].subrows[bestSub], i, best);
        ++pass.placed;
        if (!mirrored && design.onFrame && params.frameEvery > 0 &&
            pass.placed % params.frameEvery == 0) {
            writeFrame("legalizing", pass.placed, mov_.size());
        }
    }

    for (const RowTrack &R : rows_) {
        for (const Subrow &sr : R.subrows) {
            for (const Cluster &c : sr.clusters) {
                double off = 0.0;
                for (const std::size_t m : c.members) {
                    const double x = c.x + off;
                    pass.xs[m] = mirrored ? -(x + w_[m]) : x;
                    pass.ys[m] = R.y;
                    off += w_[m];
                }
            }
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        pass.movement += std::hypot(pass.xs[i] - x0_[i], pass.ys[i] - y0_[i]);
    }
    return pass;
}

// ---------------------------------------------------------------------------

AbacusLegalizer::AbacusLegalizer(const abacus::Design &design)
    : pImpl(std::make_unique<Impl>(design)) {}

AbacusLegalizer::~AbacusLegalizer() = default;

namespace {

void report(const LegalizeResult &r) {
    ktReportTable t("Legalization (Abacus)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells placed", fmt::format("{}", r.cellsPlaced)});
    t.addRow({"cells unplaced", fmt::format("{}", r.unplaced)});
    t.addRow({"squared displacement", fmt::format("{:.6}", r.totalSquaredDisplacement)});
    t.addRow({"max displacement", fmt::format("{:.6}", r.maxDisplacement)});
    t.addRow({"HPWL before", fmt::format("{:.6}", r.hpwlBefore)});
    t.addRow({"HPWL after", fmt::format("{:.6}", r.hpwlAfter)});
    t.addRow({"time (s)", fmt::format("{:.6}", r.seconds)});
    t.addRow({"overlapping pairs", fmt::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", fmt::format("{}", r.offRow)});
    t.addRow({"cells off site", fmt::format("{}", r.offSite)});
    t.addRow({"cells over macro", fmt::format("{}", r.overFixed)});
    t.addRow({"cells out of rows", fmt::format("{}", r.outOfRows)});
    t.addRow({"commit failures", fmt::format("{}", r.commitFailures)});
    t.emit();

    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }
    // A cell Abacus could not fit into any row it searched stays where global
    // placement left it, overlapping. That is a cell taller than one row (the flow
    // sends those to the multi-row legalizer instead), or one whose nearby rows
    // were full -- typically a global placement that piled cells onto macros or
    // outside the rows. Named here rather than left to be inferred from a count.
    if (r.outOfRows > 0) {
        ktlog.warning(
            "{} cell(s) could not be placed in any row and are still at their global placement "
            "positions (no room within KTPLACE_ABACUS_MAX_ROW_DIST rows, or taller than a row). "
            "The placement is not legal; see \"cells out of rows\" above.",
            r.outOfRows);
    }
}

}  // namespace

LegalizeResult AbacusLegalizer::legalize(const AbacusOptions &options) {
    LegalizeResult result = pImpl->run(options);
    report(result);
    return result;
}

ktplace::abacusSolution AbacusLegalizer::solution() const {
    abacusSolution out;
    out.xs = pImpl->finalXs;
    out.ys = pImpl->finalYs;
    return out;
}
}  // namespace ktplace
