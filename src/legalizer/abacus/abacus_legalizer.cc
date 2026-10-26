#include "legalizer/abacus/abacus_legalizer.h"

#include "datamodel/kt_hpwl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <oneapi/tbb/parallel_invoke.h>

namespace ktplace::abacus {

namespace {

constexpr double infeasible = std::numeric_limits<double>::infinity();

struct Span {
    double n = 0.0;
    double w = 0.0;
    double a = 0.0;
    double b = 0.0;

    [[nodiscard]] double getCostAt(double pos) const {
        return n * pos * pos + 2.0 * a * pos + b;
    }
    [[nodiscard]] double getIdeal() const {
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
    double delta = infeasible;
    std::size_t first = 0;
    double x = 0.0;
    Span span;
};

struct Segment {
    Interval x;
    int region = noRegion;
    std::vector<Cluster> clusters;
    double used = 0.0;

    [[nodiscard]] double getWidth() const {
        return x.getLength();
    }
    [[nodiscard]] double getFree() const {
        return x.end - used;
    }
};

struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double siteWidth = 1.0;
    std::vector<Segment> subrows;
};

struct Stats {
    std::size_t cellsPlaced = 0;
    std::size_t unplaced = 0;
    double totalSquaredDisplacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    std::size_t outOfRows = 0;
    std::size_t commitFailures = 0;
};

void report(const Stats &r) {
    ktReportTable t("Legalization (Abacus)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells placed", std::format("{}", r.cellsPlaced)});
    t.addRow({"cells unplaced", std::format("{}", r.unplaced)});
    t.addRow({"squared displacement", std::format("{:.6e}", r.totalSquaredDisplacement)});
    t.addRow({"max displacement", std::format("{:.1f}", r.maxDisplacement)});
    t.addRow({"HPWL before", std::format("{:.6e}", r.hpwlBefore)});
    t.addRow({"HPWL after", std::format("{:.6e}", r.hpwlAfter)});
    t.addRow(
        {"HPWL change", std::format("{:+.2f}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                                    (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"seconds", std::format("{:.3f}", r.seconds)});
    t.addRow({"overlapping pairs", std::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", std::format("{}", r.offRow)});
    t.addRow({"cells off site", std::format("{}", r.offSite)});
    t.addRow({"cells over macro", std::format("{}", r.overFixed)});
    t.addRow({"cells out of rows", std::format("{}", r.outOfRows)});
    t.addRow({"commit failures", std::format("{}", r.commitFailures)});
    t.emit();

    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }
    if (r.outOfRows > 0) {
        ktlog.warning(
            "{} cell(s) could not be placed in any row and are still at their global placement "
            "positions (no room within 'abacus max_row_dist' rows, or taller than a row). "
            "The placement is not legal; see \"cells out of rows\" above.",
            r.outOfRows);
    }
}

}  // namespace

class AbacusLegalizer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    Stats run(const AbacusOptions &params);

private:
    [[nodiscard]] std::vector<RowTrack> buildRows(bool mirrored) const;
    struct Pass {
        std::vector<double> xs;
        std::vector<double> ys;
        std::size_t placed = 0;
        double movement = 0.0;
    };
    [[nodiscard]] Pass legalizePass(const AbacusOptions &params, bool mirrored,
                                    std::size_t frameInterval);
    void placedPositions(const std::vector<RowTrack> &rows, bool mirrored, std::vector<double> &xs,
                         std::vector<double> &ys) const;
    [[nodiscard]] Trial getTrial(const Segment &sr, double grid, std::size_t i,
                                 const std::vector<double> &sortKey) const;
    void commit(Segment &sr, std::size_t i, const Trial &t);
    [[nodiscard]] double getHpwlOf(const std::vector<double> &x,
                                   const std::vector<double> &y) const;
    void writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                    const std::string &note, std::size_t step, std::size_t total) const;
    void selfCheck(Stats &res) const;

    static constexpr std::size_t noRow = std::numeric_limits<std::size_t>::max();

    Design &design;
    std::vector<RowTrack> tracks;
    const std::vector<std::size_t> &mov = design.cells.id;
    const std::vector<double> &cellW = design.cells.width;
    const std::vector<double> &cellH = design.cells.height;
    const std::vector<int> &cellRegion = design.cells.region;
    std::vector<double> targetX = design.cells.x;
    std::vector<double> targetY = design.cells.y;
    std::vector<double> liveX, liveY;
    const std::vector<BBox> &fixedBoxes = design.fixedBoxes;
    BBox dieBox = BBox(Point(0.0, 0.0), Point(0.0, 0.0));
    double minSubrowSites = 1.0;
};

std::vector<RowTrack> AbacusLegalizer::Impl::buildRows(bool mirrored) const {
    std::vector<RowTrack> rows;
    for (const RowInfo &ri : design.rows) {
        RowTrack r;
        r.y = ri.coordinate;
        r.height = ri.height;
        r.siteWidth = ri.getPitch();
        if (!(r.siteWidth > 0.0)) {
            r.siteWidth = 1.0;
        }
        for (const Subrow &si : ri.subrows) {
            if (!(si.x.end > si.x.start)) {
                continue;
            }
            Segment sr;
            sr.x.start = si.x.start;
            sr.x.end = si.x.end;
            sr.region = si.region;
            sr.used = sr.x.start;
            r.subrows.push_back(sr);
        }
        if (r.subrows.empty()) {
            continue;
        }
        std::stable_sort(r.subrows.begin(), r.subrows.end(),
                         [](const Segment &a, const Segment &b) {
                             return a.x.start < b.x.start;
                         });
        rows.push_back(r);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const RowTrack &a, const RowTrack &b) {
        return a.y < b.y;
    });

    for (RowTrack &r : rows) {
        std::vector<Segment> pieces;
        for (const Segment &sr : r.subrows) {
            std::vector<Interval> hit;
            for (const BBox &f : fixedBoxes) {
                if (f.upperRight.y <= r.y + 1e-9 || f.lowerLeft.y >= r.y + r.height - 1e-9) {
                    continue;
                }
                const double a = std::max(f.lowerLeft.x, sr.x.start);
                const double b = std::min(f.upperRight.x, sr.x.end);
                if (b > a) {
                    hit.emplace_back(a, b);
                }
            }
            std::stable_sort(hit.begin(), hit.end());
            double cur = sr.x.start;
            for (const auto &iv : hit) {
                if (iv.start > cur) {
                    Segment piece = sr;
                    piece.x.start = cur;
                    piece.x.end = iv.start;
                    piece.used = cur;
                    pieces.push_back(piece);
                }
                cur = std::max(cur, iv.end);
            }
            if (cur < sr.x.end) {
                Segment piece = sr;
                piece.x.start = cur;
                piece.used = cur;
                pieces.push_back(piece);
            }
        }
        r.subrows = std::move(pieces);
    }
    if (mirrored) {
        for (RowTrack &r : rows) {
            for (Segment &sr : r.subrows) {
                const double lo = -sr.x.end;
                sr.x.end = -sr.x.start;
                sr.x.start = lo;
                sr.used = lo;
            }
            std::reverse(r.subrows.begin(), r.subrows.end());
        }
    }
    for (RowTrack &r : rows) {
        std::vector<Segment> kept;
        for (Segment &piece : r.subrows) {
            if (piece.getWidth() >= r.siteWidth * minSubrowSites) {
                kept.push_back(std::move(piece));
            }
        }
        r.subrows = std::move(kept);
    }
    return rows;
}

Trial AbacusLegalizer::Impl::getTrial(const Segment &sr, double grid, std::size_t i,
                                      const std::vector<double> &sortKey) const {
    const auto position = [&](const Span &span) {
        double x = std::clamp(span.getIdeal(), sr.x.start, std::max(sr.x.start, sr.x.end - span.w));
        if (grid > 0.0) {
            x = std::round(x / grid) * grid;
            if (x + span.w > sr.x.end + 1e-9) {
                x -= grid;
            }
            if (x < sr.x.start - 1e-9) {
                x += grid;
            }
        }
        return x;
    };

    Span span{.n = 1.0, .w = cellW[i], .a = -sortKey[i], .b = sortKey[i] * sortKey[i]};
    std::size_t first = sr.clusters.size();
    double before = 0.0;
    double x = position(span);
    while (first > 0 && sr.clusters[first - 1].x + sr.clusters[first - 1].span.w > x + 1e-9) {
        const Cluster &prev = sr.clusters[first - 1];
        before += prev.span.getCostAt(prev.x);
        Span merged = prev.span;
        merged.append(span);
        span = merged;
        --first;
        x = position(span);
    }
    if (x < sr.x.start - 1e-9 || x + span.w > sr.x.end + 1e-9) {
        return {};
    }
    return {.delta = span.getCostAt(x) - before, .first = first, .x = x, .span = span};
}

void AbacusLegalizer::Impl::commit(Segment &sr, std::size_t i, const Trial &t) {
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

double AbacusLegalizer::Impl::getHpwlOf(const std::vector<double> &x,
                                        const std::vector<double> &y) const {
    return getNetHpwl(
        design.nets.size(),
        [this](std::size_t n) {
            return design.nets[n];
        },
        [&](const NetPin &pin) {
            return pin.slot == noSlot
                       ? pin.fixedPosition
                       : Point(x[pin.slot] + pin.offset.x, y[pin.slot] + pin.offset.y);
        });
}

void AbacusLegalizer::Impl::writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                                       const std::string &note, std::size_t step,
                                       std::size_t total) const {
    const std::size_t nv = design.vertexX.size();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(design.vertexX[v]);
        fy[v] = static_cast<float>(design.vertexY[v]);
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        fx[mov[i]] = static_cast<float>(xs[i]);
        fy[mov[i]] = static_cast<float>(ys[i]);
    }
    if (design.onFrame) {
        FrameInfo info;
        info.note = note;
        info.step = step;
        info.numSteps = total;
        info.hpwl = getHpwlOf(xs, ys);
        info.hpwlInitial = getHpwlOf(targetX, targetY);
        info.mandatory = true;
        design.onFrame(fx, fy, info);
    }
}

void AbacusLegalizer::Impl::selfCheck(Stats &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.commitFailures = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    res.outOfRows = 0;
    std::vector<std::size_t> rowOf(mov.size(), noRow);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        const auto it = std::lower_bound(tracks.begin(), tracks.end(), liveY[i] - eps,
                                         [](const RowTrack &r, double y) {
                                             return r.y < y;
                                         });
        if (it != tracks.end() && std::fabs(it->y - liveY[i]) <= eps) {
            rowOf[i] = static_cast<std::size_t>(it - tracks.begin());
        }
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        const RowTrack *inRow = (rowOf[i] == noRow) ? nullptr : &tracks[rowOf[i]];
        if (inRow == nullptr) {
            ++res.offRow;
            ++res.outOfRows;
            continue;
        }

        const double q = liveX[i] / inRow->siteWidth;
        const double off = std::fabs(q - std::round(q)) * inRow->siteWidth;
        if (off > 1e-6 * std::max(1.0, std::fabs(liveX[i]))) {
            ++res.offSite;
        }
        bool inside = false;
        for (const Segment &sr : inRow->subrows) {
            if (liveX[i] >= sr.x.start - eps && liveX[i] + cellW[i] <= sr.x.end + eps) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            ++res.outOfRows;
        }
    }

    std::vector<std::uint64_t> order(mov.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (rowOf[a] != rowOf[b]) {
            return rowOf[a] < rowOf[b];
        }
        return liveX[a] < liveX[b];
    });
    for (std::size_t k = 1; k < order.size(); ++k) {
        const std::size_t a = order[k - 1], b = order[k];
        if (rowOf[a] == rowOf[b] && liveX[a] + cellW[a] > liveX[b] + eps) {
            ++res.overlappingPairs;
        }
    }

    const std::vector<BBox> &fixed = fixedBoxes;
    std::vector<std::size_t> byX(mov.size());
    std::iota(byX.begin(), byX.end(), 0u);
    std::stable_sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        return liveX[a] < liveX[b];
    });
    std::size_t lo = 0;
    for (const std::size_t i : byX) {
        while (lo < fixed.size() && fixed[lo].upperRight.x <= liveX[i] + eps) {
            ++lo;
        }
        for (std::size_t f = lo; f < fixed.size() && fixed[f].lowerLeft.x < liveX[i] + cellW[i];
             ++f) {
            if (fixed[f].upperRight.x <= liveX[i] + eps) {
                continue;  // sorted by x0, so a later box can still end earlier
            }
            if (liveY[i] + cellH[i] > fixed[f].lowerLeft.y + eps &&
                liveY[i] < fixed[f].upperRight.y - eps) {
                ++res.overFixed;
            }
        }
    }
}

Stats AbacusLegalizer::Impl::run(const AbacusOptions &params) {
    Stats res;
    minSubrowSites = static_cast<double>(params.minSubrowSites);
    ScopedTimer timer("abacus");

    liveX = targetX;
    liveY = targetY;
    res.cellsPlaced = mov.size();
    if (mov.empty()) {
        return res;
    }
    res.hpwlBefore = getHpwlOf(liveX, liveY);

    tracks = buildRows(false);
    if (tracks.empty()) {
        res.unplaced = mov.size();
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }
    dieBox = design.die;

    if (design.onFrame) {
        writeFrame(liveX, liveY, "Input - global placement", 0, mov.size());
    }

    Pass forward;
    Pass backward;
    tbb::parallel_invoke(
        [&] {
            forward = legalizePass(params, false, 0);
        },
        [&] {
            backward = legalizePass(params, true, 0);
        });
    const bool useBackward =
        backward.placed > forward.placed ||
        (backward.placed == forward.placed && backward.movement < forward.movement);
    ktlog.trace("  abacus: movement {:.6e} increasing x, {:.6e} decreasing x; using {}",
                forward.movement, backward.movement, useBackward ? "decreasing" : "increasing");
    Pass &chosen = useBackward ? backward : forward;
    if (design.onFrame && params.frames > 0) {
        (void)legalizePass(params, useBackward, (mov.size() + params.frames - 1) / params.frames);
    }
    liveX = std::move(chosen.xs);
    liveY = std::move(chosen.ys);
    tracks = buildRows(false);

    for (std::size_t i = 0; i < mov.size(); ++i) {
        const double dx = liveX[i] - targetX[i];
        const double dy = liveY[i] - targetY[i];
        res.totalSquaredDisplacement += dx * dx + dy * dy;
        res.maxDisplacement = std::max(res.maxDisplacement, std::sqrt(dx * dx + dy * dy));
    }
    res.hpwlAfter = getHpwlOf(liveX, liveY);

    design.cells.x = liveX;
    design.cells.y = liveY;
    res.unplaced = mov.size() - chosen.placed;
    if (design.onFrame) {
        writeFrame(liveX, liveY, "Legal placement", chosen.placed, mov.size());
    }
    selfCheck(res);
    res.seconds = timer.getElapsedSeconds();
    return res;
}

void AbacusLegalizer::Impl::placedPositions(const std::vector<RowTrack> &rows, bool mirrored,
                                            std::vector<double> &xs,
                                            std::vector<double> &ys) const {
    for (const RowTrack &track : rows) {
        for (const Segment &sr : track.subrows) {
            for (const Cluster &c : sr.clusters) {
                double off = 0.0;
                for (const std::size_t m : c.members) {
                    const double x = c.x + off;
                    xs[m] = mirrored ? -(x + cellW[m]) : x;
                    ys[m] = track.y;
                    off += cellW[m];
                }
            }
        }
    }
}

AbacusLegalizer::Impl::Pass AbacusLegalizer::Impl::legalizePass(const AbacusOptions &params,
                                                                bool mirrored,
                                                                std::size_t frameInterval) {
    std::vector<RowTrack> rows = buildRows(mirrored);
    const double tallestRow =
        rows.empty() ? 0.0 : std::ranges::max(rows, {}, &RowTrack::height).height;
    std::vector<double> sortKey(mov.size());
    for (std::size_t i = 0; i < mov.size(); ++i) {
        sortKey[i] = mirrored ? -(targetX[i] + cellW[i]) : targetX[i];
    }
    std::vector<std::size_t> order(mov.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return sortKey[a] < sortKey[b];
    });

    Pass pass;
    pass.xs = targetX;
    pass.ys = targetY;
    for (const std::size_t i : order) {
        const auto rowGap = [&](const RowTrack &track) {
            return (targetY[i] < track.y)                  ? (track.y - targetY[i])
                   : (targetY[i] > track.y + track.height) ? (targetY[i] - (track.y + track.height))
                                                           : 0.0;
        };
        std::size_t home = 0;
        if (!rows.empty()) {
            const auto above = std::ranges::upper_bound(rows, targetY[i], {}, &RowTrack::y);
            const auto k = static_cast<std::size_t>(above - rows.begin());
            const std::size_t lo =
                k > 0 ? static_cast<std::size_t>(
                            std::ranges::lower_bound(rows, rows[k - 1].y - tallestRow, {},
                                                     &RowTrack::y) -
                            rows.begin())
                      : 0;
            const std::size_t hi = std::min(k + 1, rows.size());
            double nearest = std::numeric_limits<double>::max();
            for (std::size_t r = lo; r < hi; ++r) {
                if (rowGap(rows[r]) < nearest) {
                    nearest = rowGap(rows[r]);
                    home = r;
                }
            }
        }

        std::size_t bestRow = noRow;
        std::size_t bestSub = 0;
        double bestCost = std::numeric_limits<double>::max();
        Trial best;
        const auto tryRow = [&](std::size_t ri) {
            if (ri >= rows.size()) {
                return;
            }
            const RowTrack &track = rows[ri];
            const double dy = rowGap(track);
            for (std::size_t si = 0; si < track.subrows.size(); ++si) {
                const Segment &sr = track.subrows[si];
                if (cellH[i] > track.height + 1e-9 || sr.region != cellRegion[i] ||
                    cellW[i] > sr.getWidth() + 1e-9) {
                    continue;
                }
                const double dx = (sortKey[i] < sr.x.start) ? (sr.x.start - sortKey[i])
                                  : (sortKey[i] > sr.x.end - cellW[i])
                                      ? (sortKey[i] - (sr.x.end - cellW[i]))
                                      : 0.0;
                if (dx * dx + dy * dy >= bestCost) {
                    continue;
                }
                const Trial t = getTrial(sr, track.siteWidth, i, sortKey);
                if (!std::isfinite(t.delta)) {
                    continue;
                }
                const double moved = t.x + t.span.w - cellW[i] - sortKey[i];
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
        for (std::size_t step = 1; step < rows.size(); ++step) {
            if (params.maxRowDistance > 0 && step > params.maxRowDistance) {
                break;
            }
            const double up = home + step < rows.size() ? rows[home + step].y - rows[home].y
                                                        : std::numeric_limits<double>::max();
            const double down = step <= home ? rows[home].y - rows[home - step].y
                                             : std::numeric_limits<double>::max();
            if (std::min(up, down) * std::min(up, down) > bestCost) {
                break;
            }
            tryRow(home + step);
            if (step <= home) {
                tryRow(home - step);
            }
        }
        if (bestRow == noRow) {
            for (std::size_t ri = 0; ri < rows.size(); ++ri) {
                tryRow(ri);
            }
        }
        if (bestRow == noRow) {
            continue;
        }
        commit(rows[bestRow].subrows[bestSub], i, best);
        ++pass.placed;
        if (frameInterval > 0 && pass.placed % frameInterval == 0) {
            std::vector<double> xs = targetX;
            std::vector<double> ys = targetY;
            placedPositions(rows, mirrored, xs, ys);
            writeFrame(xs, ys, "Placing cells into rows", pass.placed, mov.size());
        }
    }

    placedPositions(rows, mirrored, pass.xs, pass.ys);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        pass.movement += std::sqrt((pass.xs[i] - targetX[i]) * (pass.xs[i] - targetX[i]) +
                                   (pass.ys[i] - targetY[i]) * (pass.ys[i] - targetY[i]));
    }
    return pass;
}

AbacusLegalizer::AbacusLegalizer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

AbacusLegalizer::~AbacusLegalizer() = default;

void AbacusLegalizer::legalize(const AbacusOptions &options) {
    report(pImpl->run(options));
}

}  // namespace ktplace::abacus
