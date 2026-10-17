#include "legalizer/abacus/abacus_legalizer.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <numeric>

namespace ktplace::abacus {

namespace {

struct FixedBox {
    double y0, y1, x0, x1;
};

constexpr double infeasible = std::numeric_limits<double>::infinity();

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
    double delta = infeasible;
    std::size_t first = 0;
    double x = 0.0;
    Span span;
};

struct Segment {
    double xlo = 0.0;
    double xhi = 0.0;
    int region = noRegion;
    std::vector<Cluster> clusters;
    double used = 0.0;

    [[nodiscard]] double width() const {
        return xhi - xlo;
    }
    [[nodiscard]] double free() const {
        return xhi - used;
    }
};

struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double siteWidth = 1.0;
    std::vector<Segment> subrows;

    [[nodiscard]] double capacity() const {
        double t = 0.0;
        for (const Segment &sr : subrows) {
            t += sr.free();
        }
        return t;
    }
};

}  // namespace

class AbacusLegalizer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    AbacusResult run(const AbacusOptions &params);

private:
    void buildRows(bool mirrored);
    struct Pass {
        std::vector<double> xs;
        std::vector<double> ys;
        std::size_t placed = 0;
        double movement = 0.0;
    };
    [[nodiscard]] Pass legalizePass(const AbacusOptions &params, bool mirrored,
                                    std::size_t frameInterval);
    void placedPositions(bool mirrored, std::vector<double> &xs, std::vector<double> &ys) const;
    [[nodiscard]] Trial trial(const Segment &sr, double grid, std::size_t i) const;
    void commit(Segment &sr, std::size_t i, const Trial &t);
    [[nodiscard]] double hpwlOf(const std::vector<double> &x, const std::vector<double> &y) const;
    void writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                    const std::string &note, std::size_t step, std::size_t total) const;
    void selfCheck(AbacusResult &res) const;

    static constexpr std::size_t noRow = std::numeric_limits<std::size_t>::max();

    Design &design;
    std::vector<RowTrack> tracks;
    std::vector<std::size_t> mov;
    std::vector<double> cellW, cellH;
    std::vector<double> targetX, targetY;
    std::vector<double> sortKey;
    std::vector<double> liveX, liveY;
    std::vector<int> cellRegion;
    std::unordered_map<std::size_t, std::size_t> slotOf;
    std::vector<FixedBox> fixedBoxes;
    DieBox dieBox = {0.0, 0.0, 0.0, 0.0};
    double minSubrowSites = 1.0;
};

void AbacusLegalizer::Impl::buildRows(bool mirrored) {
    tracks.clear();
    for (const RowInfo &ri : design.rows) {
        RowTrack r;
        r.y = ri.coordinate;
        r.height = ri.height;
        r.siteWidth = ri.pitch();
        if (!(r.siteWidth > 0.0)) {
            r.siteWidth = 1.0;
        }
        for (const Subrow &si : ri.subrows) {
            if (!(si.xhi > si.xlo)) {
                continue;
            }
            Segment sr;
            sr.xlo = si.xlo;
            sr.xhi = si.xhi;
            sr.region = si.region;
            sr.used = sr.xlo;
            r.subrows.push_back(sr);
        }
        if (r.subrows.empty()) {
            continue;
        }
        std::stable_sort(r.subrows.begin(), r.subrows.end(),
                         [](const Segment &a, const Segment &b) {
                             return a.xlo < b.xlo;
                         });
        tracks.push_back(r);
    }
    std::stable_sort(tracks.begin(), tracks.end(), [](const RowTrack &a, const RowTrack &b) {
        return a.y < b.y;
    });

    for (RowTrack &r : tracks) {
        std::vector<Segment> pieces;
        for (const Segment &sr : r.subrows) {
            std::vector<std::pair<double, double>> hit;
            for (const FixedBox &f : fixedBoxes) {
                if (f.y1 <= r.y + 1e-9 || f.y0 >= r.y + r.height - 1e-9) {
                    continue;
                }
                const double a = std::max(f.x0, sr.xlo);
                const double b = std::min(f.x1, sr.xhi);
                if (b > a) {
                    hit.emplace_back(a, b);
                }
            }
            std::stable_sort(hit.begin(), hit.end());
            double cur = sr.xlo;
            for (const auto &iv : hit) {
                if (iv.first > cur) {
                    Segment piece = sr;
                    piece.xlo = cur;
                    piece.xhi = iv.first;
                    piece.used = cur;
                    pieces.push_back(piece);
                }
                cur = std::max(cur, iv.second);
            }
            if (cur < sr.xhi) {
                Segment piece = sr;
                piece.xlo = cur;
                piece.used = cur;
                pieces.push_back(piece);
            }
        }
        r.subrows = std::move(pieces);
    }
    if (mirrored) {
        for (RowTrack &r : tracks) {
            for (Segment &sr : r.subrows) {
                const double lo = -sr.xhi;
                sr.xhi = -sr.xlo;
                sr.xlo = lo;
                sr.used = lo;
            }
            std::reverse(r.subrows.begin(), r.subrows.end());
        }
    }
    for (RowTrack &r : tracks) {
        std::vector<Segment> kept;
        for (Segment &piece : r.subrows) {
            if (piece.width() >= r.siteWidth * minSubrowSites) {
                kept.push_back(std::move(piece));
            }
        }
        r.subrows = std::move(kept);
    }
}

Trial AbacusLegalizer::Impl::trial(const Segment &sr, double grid, std::size_t i) const {
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

    Span span{1.0, cellW[i], -sortKey[i], sortKey[i] * sortKey[i]};
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

double AbacusLegalizer::Impl::hpwlOf(const std::vector<double> &x,
                                     const std::vector<double> &y) const {
    double total = 0.0;
    for (const Net &net : design.nets) {
        if (net.pins.empty()) {
            continue;
        }
        double ax = std::numeric_limits<double>::max(), bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max(), by = -std::numeric_limits<double>::max();
        for (const std::size_t pinId : net.pins) {
            const Pin &pin = design.pins[pinId];
            const auto it = slotOf.find(pin.cellId);
            double cx = 0.0, cy = 0.0;
            if (it != slotOf.end()) {
                cx = x[it->second] + pin.offsetX;
                cy = y[it->second] + pin.offsetY;
            } else {
                const Cell &c = design.cells[pin.cellId];
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

void AbacusLegalizer::Impl::writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                                       const std::string &note, std::size_t step,
                                       std::size_t total) const {
    const std::size_t nv = design.cells.size();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(design.cells[v].x);
        fy[v] = static_cast<float>(design.cells[v].y);
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
        info.hpwl = hpwlOf(xs, ys);
        info.hpwlInitial = hpwlOf(targetX, targetY);
        info.mandatory = true;
        design.onFrame(fx, fy, info);
    }
}

void AbacusLegalizer::Impl::selfCheck(AbacusResult &res) const {
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
            if (liveX[i] >= sr.xlo - eps && liveX[i] + cellW[i] <= sr.xhi + eps) {
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

    const std::vector<FixedBox> &fixed = fixedBoxes;
    std::vector<std::size_t> byX(mov.size());
    std::iota(byX.begin(), byX.end(), 0u);
    std::stable_sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        return liveX[a] < liveX[b];
    });
    std::size_t lo = 0;
    for (const std::size_t i : byX) {
        while (lo < fixed.size() && fixed[lo].x1 <= liveX[i] + eps) {
            ++lo;
        }
        for (std::size_t f = lo; f < fixed.size() && fixed[f].x0 < liveX[i] + cellW[i]; ++f) {
            if (fixed[f].x1 <= liveX[i] + eps) {
                continue;  // sorted by x0, so a later box can still end earlier
            }
            if (liveY[i] + cellH[i] > fixed[f].y0 + eps && liveY[i] < fixed[f].y1 - eps) {
                ++res.overFixed;
            }
        }
    }
}

AbacusResult AbacusLegalizer::Impl::run(const AbacusOptions &params) {
    AbacusResult res;
    minSubrowSites = static_cast<double>(params.minSubrowSites);
    ScopedTimer timer("abacus");

    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        const Cell &cell = design.cells[v];
        if (cell.isFixed || cell.isTerminal) {
            continue;
        }
        const std::size_t slot = mov.size();
        mov.push_back(v);
        slotOf[v] = slot;
        cellW.push_back(cell.width);
        cellH.push_back(cell.height);
        cellRegion.push_back(cell.region >= 0 && static_cast<std::size_t>(cell.region) <
                                                     design.fences.numRegions()
                                 ? cell.region
                                 : noRegion);
        targetX.push_back(cell.x);
        targetY.push_back(cell.y);
    }
    liveX = targetX;
    liveY = targetY;
    res.cellsPlaced = mov.size();
    if (mov.empty()) {
        return res;
    }
    res.hpwlBefore = hpwlOf(liveX, liveY);

    for (const auto &vert : design.cells) {
        if (!vert.isFixed) {
            continue;
        }
        fixedBoxes.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
    }

    buildRows(false);
    if (tracks.empty()) {
        res.unplaced = mov.size();
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }
    dieBox = design.die;

    if (design.onFrame) {
        writeFrame(liveX, liveY, "Input - global placement", 0, mov.size());
    }

    Pass forward = legalizePass(params, false, 0);
    Pass backward = legalizePass(params, true, 0);
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
    buildRows(false);

    for (std::size_t i = 0; i < mov.size(); ++i) {
        const double dx = liveX[i] - targetX[i];
        const double dy = liveY[i] - targetY[i];
        res.totalSquaredDisplacement += dx * dx + dy * dy;
        res.maxDisplacement = std::max(res.maxDisplacement, std::sqrt(dx * dx + dy * dy));
    }
    res.hpwlAfter = hpwlOf(liveX, liveY);

    for (std::size_t i = 0; i < mov.size(); ++i) {
        design.cells[mov[i]].x = liveX[i];
        design.cells[mov[i]].y = liveY[i];
    }
    res.unplaced = mov.size() - chosen.placed;
    if (design.onFrame) {
        writeFrame(liveX, liveY, "Legal placement", chosen.placed, mov.size());
    }
    selfCheck(res);
    res.seconds = timer.elapsedSeconds();
    return res;
}

void AbacusLegalizer::Impl::placedPositions(bool mirrored, std::vector<double> &xs,
                                            std::vector<double> &ys) const {
    for (const RowTrack &track : tracks) {
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
    buildRows(mirrored);
    sortKey.resize(mov.size());
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
        double nearest = std::numeric_limits<double>::max();
        for (std::size_t r = 0; r < tracks.size(); ++r) {
            if (rowGap(tracks[r]) < nearest) {
                nearest = rowGap(tracks[r]);
                home = r;
            }
        }

        std::size_t bestRow = noRow;
        std::size_t bestSub = 0;
        double bestCost = std::numeric_limits<double>::max();
        Trial best;
        const auto tryRow = [&](std::size_t ri) {
            if (ri >= tracks.size()) {
                return;
            }
            const RowTrack &track = tracks[ri];
            const double dy = rowGap(track);
            for (std::size_t si = 0; si < track.subrows.size(); ++si) {
                const Segment &sr = track.subrows[si];
                if (cellH[i] > track.height + 1e-9 || sr.region != cellRegion[i] ||
                    cellW[i] > sr.width() + 1e-9) {
                    continue;
                }
                const double dx = (sortKey[i] < sr.xlo) ? (sr.xlo - sortKey[i])
                                  : (sortKey[i] > sr.xhi - cellW[i])
                                      ? (sortKey[i] - (sr.xhi - cellW[i]))
                                      : 0.0;
                if (dx * dx + dy * dy >= bestCost) {
                    continue;
                }
                const Trial t = trial(sr, track.siteWidth, i);
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
        for (std::size_t step = 1; step < tracks.size(); ++step) {
            if (params.maxRowDistance > 0 && step > params.maxRowDistance) {
                break;
            }
            const double up = home + step < tracks.size() ? tracks[home + step].y - tracks[home].y
                                                          : std::numeric_limits<double>::max();
            const double down = step <= home ? tracks[home].y - tracks[home - step].y
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
            for (std::size_t ri = 0; ri < tracks.size(); ++ri) {
                tryRow(ri);
            }
        }
        if (bestRow == noRow) {
            continue;
        }
        commit(tracks[bestRow].subrows[bestSub], i, best);
        ++pass.placed;
        if (frameInterval > 0 && pass.placed % frameInterval == 0) {
            std::vector<double> xs = targetX;
            std::vector<double> ys = targetY;
            placedPositions(mirrored, xs, ys);
            writeFrame(xs, ys, "Placing cells into rows", pass.placed, mov.size());
        }
    }

    placedPositions(mirrored, pass.xs, pass.ys);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        pass.movement += std::sqrt((pass.xs[i] - targetX[i]) * (pass.xs[i] - targetX[i]) +
                                   (pass.ys[i] - targetY[i]) * (pass.ys[i] - targetY[i]));
    }
    return pass;
}

AbacusLegalizer::AbacusLegalizer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

AbacusLegalizer::~AbacusLegalizer() = default;

AbacusResult AbacusLegalizer::legalize(const AbacusOptions &options) {
    return pImpl->run(options);
}

}  // namespace ktplace::abacus
