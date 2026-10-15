// @file multirow_legalizer.cc
// Multi-row height legalization: Abacus extended to cells spanning several rows
// (Chow, Pui & Young, DAC 2016), after a macro pre-pass for very tall cells

#include "legalizer/multirow/multirow_legalizer.h"

#include "legalizer/multirow/multirow_design.h"
#include "util/kt_rowFence.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

constexpr double kEps = 1e-9;
constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();

struct Box {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

using Intervals = std::vector<std::pair<double, double>>;

Intervals subtract(const Intervals &from, double lo, double hi) {
    Intervals out;
    for (const auto &[s, e] : from) {
        if (hi <= s || lo >= e) {
            out.emplace_back(s, e);
            continue;
        }
        if (lo > s) {
            out.emplace_back(s, lo);
        }
        if (hi < e) {
            out.emplace_back(hi, e);
        }
    }
    return out;
}

struct Lane {
    std::size_t row = 0;
    double y = 0.0;
    double height = 0.0;
    double xlo = 0.0;
    double xhi = 0.0;
    double pitch = 0.0;
    int region = multirow::Fences::kNoRegion;
    std::vector<std::uint32_t> clusters;
};

struct Extent {
    std::uint32_t lane = 0;
    double lo = 0.0;
    double hi = 0.0;
};

struct Cluster {
    double n = 0.0;
    double a = 0.0;
    double b = 0.0;
    double x = 0.0;
    std::uint32_t root = kNone;
    std::vector<Extent> extents;

    [[nodiscard]] double ideal() const {
        return n > 0.0 ? -a / n : 0.0;
    }
    [[nodiscard]] const Extent *in(std::uint32_t lane) const {
        for (const Extent &e : extents) {
            if (e.lane == lane) {
                return &e;
            }
        }
        return nullptr;
    }
    [[nodiscard]] Extent *in(std::uint32_t lane) {
        for (Extent &e : extents) {
            if (e.lane == lane) {
                return &e;
            }
        }
        return nullptr;
    }
};

struct Slot {
    std::vector<std::uint32_t> lanes;
    double lo = 0.0;
    double hi = 0.0;
};

}  // namespace

class MultiRowLegalizer::Impl {
public:
    explicit Impl(const multirow::Design &d) : design(d) {}

    MultiRowLegalizeResult run(const MultiRowOptions &params);

    std::vector<double> xs;
    std::vector<double> ys;

private:
    void legalizeMacros(const std::vector<std::uint32_t> &macros, MultiRowLegalizeResult &res);
    void buildLanes();
    [[nodiscard]] std::vector<Slot> slots(std::size_t bottom, std::size_t span, int region,
                                          double width) const;
    [[nodiscard]] std::size_t spanOf(double height) const;
    [[nodiscard]] std::vector<std::uint32_t> fillGaps(std::vector<std::uint32_t> pending,
                                                      const std::vector<std::uint32_t> &placed,
                                                      std::vector<std::size_t> &bottomRow);

    [[nodiscard]] double offset(std::uint32_t cell) const;
    [[nodiscard]] double place(const Cluster &c, bool &feasible) const;
    [[nodiscard]] std::size_t indexIn(std::uint32_t lane, std::uint32_t cluster) const;
    std::uint32_t insert(std::uint32_t cell, const Slot &slot, double target, bool &feasible);
    void merge(std::uint32_t left, std::uint32_t right);

    void touchCluster(std::uint32_t id);
    void touchLane(std::uint32_t id);
    void setParent(std::uint32_t cell, std::uint32_t parent, double delta);
    void rollback();
    void forget();

    const multirow::Design &design;
    double rowHeight = 0.0;
    std::vector<multirow::RowInfo> rows;
    std::vector<Box> blocked;
    std::vector<Lane> lanes;
    std::vector<std::vector<std::uint32_t>> lanesOfRow;
    std::vector<Cluster> clusters;
    std::vector<std::uint32_t> parent;
    std::vector<double> delta;
    std::vector<std::uint32_t> clusterOf;

    std::vector<std::pair<std::uint32_t, Cluster>> savedClusters;
    std::vector<std::pair<std::uint32_t, std::vector<std::uint32_t>>> savedLanes;
    std::vector<std::tuple<std::uint32_t, std::uint32_t, double>> savedParents;
    std::size_t savedClusterCount = 0;
    std::vector<char> clusterTouched;
    std::vector<char> laneTouched;
};

std::vector<std::uint32_t> MultiRowLegalizer::Impl::fillGaps(
    std::vector<std::uint32_t> pending, const std::vector<std::uint32_t> &placed,
    std::vector<std::size_t> &bottomRow) {
    std::vector<Intervals> used(lanes.size());
    const auto occupy = [&](std::uint32_t v) {
        const multirow::Cell &c = design.cells[v];
        const std::size_t span = spanOf(c.height);
        for (std::size_t k = bottomRow[v]; k < bottomRow[v] + span && k < rows.size(); ++k) {
            for (const std::uint32_t l : lanesOfRow[k]) {
                if (lanes[l].xlo <= xs[v] + kEps && xs[v] + c.width <= lanes[l].xhi + kEps) {
                    Intervals &u = used[l];
                    const std::pair<double, double> taken{xs[v], xs[v] + c.width};
                    u.insert(std::lower_bound(u.begin(), u.end(), taken), taken);
                    break;
                }
            }
        }
    };
    for (const std::uint32_t v : placed) {
        occupy(v);
    }

    std::stable_sort(pending.begin(), pending.end(), [&](std::uint32_t a, std::uint32_t b) {
        const multirow::Cell &ca = design.cells[a];
        const multirow::Cell &cb = design.cells[b];
        return ca.width * ca.height > cb.width * cb.height;
    });
    std::vector<std::uint32_t> filled;
    std::vector<std::size_t> byDistance(rows.size());
    for (const std::uint32_t v : pending) {
        const multirow::Cell &cell = design.cells[v];
        const std::size_t span = spanOf(cell.height);
        const int region = cell.regionId >= 0 && static_cast<std::size_t>(cell.regionId) <
                                                     design.fences.numRegions()
                               ? cell.regionId
                               : multirow::Fences::kNoRegion;
        std::iota(byDistance.begin(), byDistance.end(), 0u);
        std::sort(byDistance.begin(), byDistance.end(), [&](std::size_t a, std::size_t b) {
            return std::abs(rows[a].coordinate - cell.y) < std::abs(rows[b].coordinate - cell.y);
        });
        double bestCost = std::numeric_limits<double>::max();
        double bestX = 0.0;
        std::size_t bestRow = 0;
        for (const std::size_t r : byDistance) {
            const double dy = rows[r].coordinate - cell.y;
            if (dy * dy >= bestCost) {
                break;
            }
            for (const Slot &slot : slots(r, span, region, cell.width)) {
                Intervals free{{slot.lo, slot.hi}};
                for (const std::uint32_t l : slot.lanes) {
                    for (const auto &[a, b] : used[l]) {
                        if (b > slot.lo && a < slot.hi) {
                            free = subtract(free, a, b);
                        }
                    }
                }
                const double pitch = lanes[slot.lanes.front()].pitch;
                for (const auto &[lo, hi] : free) {
                    if (hi - lo < cell.width - kEps) {
                        continue;
                    }
                    double x = std::clamp(cell.x, lo, hi - cell.width);
                    if (pitch > 0.0) {
                        x = std::round(x / pitch) * pitch;
                        if (x + cell.width > hi + kEps) {
                            x -= pitch;
                        }
                        if (x < lo - kEps) {
                            continue;
                        }
                    }
                    const double cost = (x - cell.x) * (x - cell.x) + dy * dy;
                    if (cost < bestCost) {
                        bestCost = cost;
                        bestX = x;
                        bestRow = r;
                    }
                }
            }
        }
        if (bestCost == std::numeric_limits<double>::max()) {
            continue;
        }
        xs[v] = bestX;
        ys[v] = rows[bestRow].coordinate;
        bottomRow[v] = bestRow;
        occupy(v);
        filled.push_back(v);
    }
    return filled;
}

std::size_t MultiRowLegalizer::Impl::spanOf(double height) const {
    return static_cast<std::size_t>(std::max(1.0, std::ceil(height / rowHeight - 1e-6)));
}

void MultiRowLegalizer::Impl::legalizeMacros(const std::vector<std::uint32_t> &macros,
                                             MultiRowLegalizeResult &res) {
    std::vector<std::uint32_t> order = macros;
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const multirow::Cell &ca = design.cells[a];
        const multirow::Cell &cb = design.cells[b];
        return ca.width * ca.height > cb.width * cb.height;
    });
    std::vector<std::size_t> byDistance(rows.size());
    for (const std::uint32_t v : order) {
        const multirow::Cell &cell = design.cells[v];
        std::iota(byDistance.begin(), byDistance.end(), 0u);
        std::sort(byDistance.begin(), byDistance.end(), [&](std::size_t a, std::size_t b) {
            return std::abs(rows[a].coordinate - cell.y) < std::abs(rows[b].coordinate - cell.y);
        });
        const std::size_t span = spanOf(cell.height);
        double bestCost = std::numeric_limits<double>::max();
        Box best;
        for (const std::size_t r : byDistance) {
            const double dy = rows[r].coordinate - cell.y;
            if (dy * dy >= bestCost) {
                break;
            }
            if (r + span > rows.size()) {
                continue;
            }
            Intervals free;
            for (const multirow::Subrow &s : rows[r].subrows) {
                free.emplace_back(s.xlo, s.xhi);
            }
            std::sort(free.begin(), free.end());
            bool contiguous = true;
            for (std::size_t k = r + 1; k < r + span && contiguous; ++k) {
                contiguous = std::abs(rows[k].coordinate -
                                      (rows[k - 1].coordinate + rows[k - 1].height)) < 1e-6;
                Intervals here;
                for (const multirow::Subrow &s : rows[k].subrows) {
                    here.emplace_back(s.xlo, s.xhi);
                }
                std::sort(here.begin(), here.end());
                Intervals both;
                std::size_t i = 0;
                std::size_t j = 0;
                while (i < free.size() && j < here.size()) {
                    const double lo = std::max(free[i].first, here[j].first);
                    const double hi = std::min(free[i].second, here[j].second);
                    if (hi > lo) {
                        both.emplace_back(lo, hi);
                    }
                    (free[i].second < here[j].second) ? ++i : ++j;
                }
                free = std::move(both);
            }
            if (!contiguous) {
                continue;
            }
            const double y0 = rows[r].coordinate;
            const double y1 = y0 + cell.height;
            for (const Box &b : blocked) {
                if (b.y1 > y0 + kEps && b.y0 < y1 - kEps) {
                    free = subtract(free, b.x0, b.x1);
                }
            }
            const double pitch = rows[r].pitch();
            for (const auto &[lo, hi] : free) {
                if (hi - lo < cell.width - kEps) {
                    continue;
                }
                double x = std::clamp(cell.x, lo, hi - cell.width);
                if (pitch > 0.0) {
                    x = lo + std::round((x - lo) / pitch) * pitch;
                    if (x + cell.width > hi + kEps) {
                        x -= pitch;
                    }
                    if (x < lo - kEps) {
                        continue;
                    }
                }
                const double cost = (x - cell.x) * (x - cell.x) + dy * dy;
                if (cost < bestCost) {
                    bestCost = cost;
                    best = {x, y0, x + cell.width, y1};
                }
            }
        }
        if (bestCost == std::numeric_limits<double>::max()) {
            continue;
        }
        xs[v] = best.x0;
        ys[v] = best.y0;
        blocked.push_back(best);
        ++res.macrosPlaced;
    }
}

void MultiRowLegalizer::Impl::buildLanes() {
    std::vector<FenceRect> fenceRects;
    for (int id = 0; id < static_cast<int>(design.fences.numRegions()); ++id) {
        for (const multirow::Rect &rc : design.fences.region(id).rects) {
            fenceRects.push_back({rc.xlo, rc.ylo, rc.xhi, rc.yhi, id});
        }
    }
    lanesOfRow.assign(rows.size(), {});
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const multirow::RowInfo &row = rows[r];
        const double y0 = row.coordinate;
        const double y1 = y0 + row.height;
        Intervals free;
        for (const multirow::Subrow &s : row.subrows) {
            if (s.xhi > s.xlo) {
                free.emplace_back(s.xlo, s.xhi);
            }
        }
        for (const Box &b : blocked) {
            if (b.y1 > y0 + kEps && b.y0 < y1 - kEps) {
                free = subtract(free, b.x0, b.x1);
            }
        }
        std::sort(free.begin(), free.end());
        for (const auto &[lo, hi] : free) {
            for (const RowPiece &piece :
                 splitRowByFences(lo, hi, y0, y1, row.pitch(), fenceRects)) {
                if (piece.xhi - piece.xlo < std::max(row.pitch(), kEps)) {
                    continue;
                }
                lanesOfRow[r].push_back(static_cast<std::uint32_t>(lanes.size()));
                lanes.push_back(
                    {r, y0, row.height, piece.xlo, piece.xhi, row.pitch(), piece.region, {}});
            }
        }
    }
}

std::vector<Slot> MultiRowLegalizer::Impl::slots(std::size_t bottom, std::size_t span, int region,
                                                 double width) const {
    std::vector<Slot> out;
    if (bottom + span > rows.size()) {
        return out;
    }
    for (std::size_t k = bottom + 1; k < bottom + span; ++k) {
        if (std::abs(rows[k].coordinate - (rows[k - 1].coordinate + rows[k - 1].height)) > 1e-6) {
            return out;
        }
    }
    for (const std::uint32_t l : lanesOfRow[bottom]) {
        if (lanes[l].region == region) {
            out.push_back({{l}, lanes[l].xlo, lanes[l].xhi});
        }
    }
    for (std::size_t k = bottom + 1; k < bottom + span && !out.empty(); ++k) {
        std::vector<Slot> next;
        for (const Slot &s : out) {
            for (const std::uint32_t l : lanesOfRow[k]) {
                const Lane &lane = lanes[l];
                const double lo = std::max(s.lo, lane.xlo);
                const double hi = std::min(s.hi, lane.xhi);
                if (lane.region == region && hi - lo >= width - kEps) {
                    Slot grown = s;
                    grown.lanes.push_back(l);
                    grown.lo = lo;
                    grown.hi = hi;
                    next.push_back(std::move(grown));
                }
            }
        }
        out = std::move(next);
    }
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const Slot &s) {
                                 return s.hi - s.lo < width - kEps;
                             }),
              out.end());
    return out;
}

double MultiRowLegalizer::Impl::offset(std::uint32_t cell) const {
    double sum = 0.0;
    for (std::uint32_t c = cell; c != kNone; c = parent[c]) {
        sum += delta[c];
    }
    return sum;
}

double MultiRowLegalizer::Impl::place(const Cluster &c, bool &feasible) const {
    double lo = -std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::max();
    double pitch = 0.0;
    for (const Extent &e : c.extents) {
        const Lane &lane = lanes[e.lane];
        lo = std::max(lo, lane.xlo - e.lo);
        hi = std::min(hi, lane.xhi - e.hi);
        pitch = std::max(pitch, lane.pitch);
    }
    feasible = lo <= hi + kEps;
    double x = std::clamp(c.ideal(), lo, std::max(lo, hi));
    if (pitch > 0.0) {
        x = std::round(x / pitch) * pitch;
        if (x > hi + kEps) {
            x -= pitch;
        }
        if (x < lo - kEps) {
            x += pitch;
        }
        feasible = feasible && x >= lo - kEps && x <= hi + kEps;
    }
    return x;
}

std::size_t MultiRowLegalizer::Impl::indexIn(std::uint32_t lane, std::uint32_t cluster) const {
    const std::vector<std::uint32_t> &list = lanes[lane].clusters;
    for (std::size_t k = list.size(); k-- > 0;) {
        if (list[k] == cluster) {
            return k;
        }
    }
    return list.size();
}

void MultiRowLegalizer::Impl::touchCluster(std::uint32_t id) {
    if (id < savedClusterCount && !clusterTouched[id]) {
        clusterTouched[id] = 1;
        savedClusters.emplace_back(id, clusters[id]);
    }
}

void MultiRowLegalizer::Impl::touchLane(std::uint32_t id) {
    if (!laneTouched[id]) {
        laneTouched[id] = 1;
        savedLanes.emplace_back(id, lanes[id].clusters);
    }
}

void MultiRowLegalizer::Impl::setParent(std::uint32_t cell, std::uint32_t newParent,
                                        double newDelta) {
    savedParents.emplace_back(cell, parent[cell], delta[cell]);
    parent[cell] = newParent;
    delta[cell] = newDelta;
}

void MultiRowLegalizer::Impl::rollback() {
    for (auto it = savedParents.rbegin(); it != savedParents.rend(); ++it) {
        parent[std::get<0>(*it)] = std::get<1>(*it);
        delta[std::get<0>(*it)] = std::get<2>(*it);
    }
    for (auto &[id, saved] : savedLanes) {
        lanes[id].clusters = std::move(saved);
    }
    for (auto &[id, saved] : savedClusters) {
        clusters[id] = std::move(saved);
    }
    clusters.resize(savedClusterCount);
    forget();
}

void MultiRowLegalizer::Impl::forget() {
    for (const auto &[id, saved] : savedClusters) {
        clusterTouched[id] = 0;
    }
    for (const auto &[id, saved] : savedLanes) {
        laneTouched[id] = 0;
    }
    savedClusters.clear();
    savedLanes.clear();
    savedParents.clear();
    savedClusterCount = clusters.size();
    clusterTouched.resize(savedClusterCount, 0);
}

void MultiRowLegalizer::Impl::merge(std::uint32_t left, std::uint32_t right) {
    touchCluster(left);
    touchCluster(right);
    Cluster &l = clusters[left];
    Cluster &r = clusters[right];
    double shift = -std::numeric_limits<double>::max();
    for (const Extent &e : r.extents) {
        if (const Extent *mine = l.in(e.lane)) {
            shift = std::max(shift, mine->hi - e.lo);
        }
    }
    if (shift == -std::numeric_limits<double>::max()) {
        shift = r.x - l.x;
    }
    l.a += r.a + r.n * shift;
    l.b += r.b + 2.0 * shift * r.a + r.n * shift * shift;
    l.n += r.n;
    for (const Extent &e : r.extents) {
        touchLane(e.lane);
        std::vector<std::uint32_t> &list = lanes[e.lane].clusters;
        const std::size_t at = indexIn(e.lane, right);
        if (Extent *mine = l.in(e.lane)) {
            mine->lo = std::min(mine->lo, e.lo + shift);
            mine->hi = std::max(mine->hi, e.hi + shift);
            if (at < list.size()) {
                list.erase(list.begin() + static_cast<long>(at));
            }
        } else {
            l.extents.push_back({e.lane, e.lo + shift, e.hi + shift});
            if (at < list.size()) {
                list[at] = left;
            }
        }
    }
    setParent(r.root, l.root, delta[r.root] + shift - delta[l.root]);
    r.extents.clear();
    r.n = 0.0;
}

std::uint32_t MultiRowLegalizer::Impl::insert(std::uint32_t cell, const Slot &slot, double target,
                                              bool &feasible) {
    const multirow::Cell &c = design.cells[cell];
    Cluster fresh;
    fresh.n = 1.0;
    fresh.a = -target;
    fresh.b = target * target;
    fresh.root = cell;
    for (const std::uint32_t l : slot.lanes) {
        fresh.extents.push_back({l, 0.0, c.width});
    }
    std::uint32_t id = static_cast<std::uint32_t>(clusters.size());
    clusters.push_back(std::move(fresh));
    clusterTouched.push_back(0);
    setParent(cell, kNone, 0.0);
    for (const std::uint32_t l : slot.lanes) {
        touchLane(l);
        lanes[l].clusters.push_back(id);
    }

    for (;;) {
        touchCluster(id);
        const double x = place(clusters[id], feasible);
        if (!feasible) {
            return id;
        }
        clusters[id].x = x;
        std::uint32_t neighbour = kNone;
        bool neighbourIsLeft = false;
        for (const Extent &e : clusters[id].extents) {
            const std::vector<std::uint32_t> &list = lanes[e.lane].clusters;
            const std::size_t at = indexIn(e.lane, id);
            if (at > 0) {
                const Cluster &p = clusters[list[at - 1]];
                const Extent *pe = p.in(e.lane);
                if (p.x + pe->hi > x + e.lo + kEps) {
                    neighbour = list[at - 1];
                    neighbourIsLeft = true;
                    break;
                }
            }
            if (at + 1 < list.size()) {
                const Cluster &s = clusters[list[at + 1]];
                const Extent *se = s.in(e.lane);
                if (x + e.hi > s.x + se->lo + kEps) {
                    neighbour = list[at + 1];
                    neighbourIsLeft = false;
                    break;
                }
            }
        }
        if (neighbour == kNone) {
            return id;
        }
        if (neighbourIsLeft) {
            merge(neighbour, id);
            id = neighbour;
        } else {
            merge(id, neighbour);
        }
    }
}

MultiRowLegalizeResult MultiRowLegalizer::Impl::run(const MultiRowOptions &params) {
    ScopedTimer timer("multirow-legalize");
    MultiRowLegalizeResult res;

    rows = design.rows;
    std::sort(rows.begin(), rows.end(), [](const multirow::RowInfo &a, const multirow::RowInfo &b) {
        return a.coordinate < b.coordinate;
    });
    rowHeight = std::numeric_limits<double>::max();
    for (const multirow::RowInfo &r : rows) {
        if (r.height > 0.0 && !r.subrows.empty()) {
            rowHeight = std::min(rowHeight, r.height);
        }
    }

    const std::size_t n = design.cells.size();
    xs.resize(n);
    ys.resize(n);
    std::vector<std::uint32_t> macros;
    std::vector<std::uint32_t> cells;
    for (std::uint32_t v = 0; v < n; ++v) {
        const multirow::Cell &c = design.cells[v];
        xs[v] = c.x;
        ys[v] = c.y;
        if (c.isFixed || c.isTerminal) {
            if (c.width > 0.0 && c.height > 0.0) {
                blocked.push_back({c.x, c.y, c.x + c.width, c.y + c.height});
            }
            continue;
        }
        ++res.movable;
        if (c.height > 1.5 * rowHeight) {
            ++res.multiRow;
        }
        if (spanOf(c.height) > params.macroRows) {
            macros.push_back(v);
        } else {
            cells.push_back(v);
        }
    }
    res.macros = macros.size();
    res.hpwlBefore = multirow::netlistHPWL(design.cells, design.nets, design.pins, xs, ys);
    if (rows.empty()) {
        res.unplaced = res.movable;
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }

    legalizeMacros(macros, res);
    buildLanes();

    parent.assign(n, kNone);
    delta.assign(n, 0.0);
    clusterOf.assign(n, kNone);
    laneTouched.assign(lanes.size(), 0);
    forget();

    std::stable_sort(cells.begin(), cells.end(), [&](std::uint32_t a, std::uint32_t b) {
        return design.cells[a].x < design.cells[b].x;
    });
    std::vector<double> rowY(rows.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        rowY[r] = rows[r].coordinate;
    }

    std::vector<std::uint32_t> placedCells;
    std::vector<std::uint32_t> pending;
    std::vector<std::size_t> bottomRow(n, 0);
    for (const std::uint32_t v : cells) {
        const multirow::Cell &cell = design.cells[v];
        const std::size_t span = spanOf(cell.height);
        const int region = cell.regionId >= 0 && static_cast<std::size_t>(cell.regionId) <
                                                     design.fences.numRegions()
                               ? cell.regionId
                               : multirow::Fences::kNoRegion;
        const std::size_t home = static_cast<std::size_t>(
            std::lower_bound(rowY.begin(), rowY.end(), cell.y) - rowY.begin());

        double bestCost = std::numeric_limits<double>::max();
        Slot best;
        std::size_t bestRow = 0;
        const auto tryRow = [&](std::size_t r) {
            const double dy = rows[r].coordinate - cell.y;
            for (const Slot &slot : slots(r, span, region, cell.width)) {
                const double dx = cell.x < slot.lo                ? slot.lo - cell.x
                                  : cell.x > slot.hi - cell.width ? cell.x - (slot.hi - cell.width)
                                                                  : 0.0;
                if (dx * dx + dy * dy >= bestCost) {
                    continue;
                }
                bool feasible = false;
                const std::uint32_t id = insert(v, slot, cell.x, feasible);
                if (feasible) {
                    const double moved = clusters[id].x + offset(v) - cell.x;
                    const double cost = moved * moved + dy * dy;
                    if (cost < bestCost) {
                        bestCost = cost;
                        best = slot;
                        bestRow = r;
                    }
                }
                rollback();
            }
        };
        const auto rowDistance = [&](std::size_t r) {
            return std::abs(rows[r].coordinate - cell.y);
        };
        std::size_t up = std::min(home, rows.size() - 1);
        std::size_t down = up;
        bool upOpen = true;
        bool downOpen = up > 0;
        if (downOpen) {
            --down;
        }
        while (upOpen || downOpen) {
            const bool takeUp = upOpen && (!downOpen || rowDistance(up) <= rowDistance(down));
            const std::size_t r = takeUp ? up : down;
            const double d = rowDistance(r);
            if (d * d >= bestCost) {
                if (takeUp) {
                    upOpen = false;
                } else {
                    downOpen = false;
                }
                continue;
            }
            tryRow(r);
            if (takeUp) {
                upOpen = ++up < rows.size();
            } else {
                downOpen = down-- > 0;
            }
        }
        if (bestCost == std::numeric_limits<double>::max()) {
            pending.push_back(v);
            continue;
        }
        bool feasible = false;
        insert(v, best, cell.x, feasible);
        forget();
        bottomRow[v] = bestRow;
        placedCells.push_back(v);
        ++res.placed;
        if (cell.height > 1.5 * rowHeight) {
            ++res.multiRowPlaced;
        }
    }

    std::vector<double> clusterX(n, 0.0);
    for (const Cluster &c : clusters) {
        if (c.n > 0.0) {
            clusterX[c.root] = c.x;
        }
    }
    for (const std::uint32_t v : placedCells) {
        std::uint32_t root = v;
        double sum = 0.0;
        for (std::uint32_t c = v; c != kNone; c = parent[c]) {
            sum += delta[c];
            root = c;
        }
        xs[v] = clusterX[root] + sum;
        ys[v] = rows[bottomRow[v]].coordinate;
    }

    for (const std::uint32_t v : fillGaps(pending, placedCells, bottomRow)) {
        ++res.placed;
        if (design.cells[v].height > 1.5 * rowHeight) {
            ++res.multiRowPlaced;
        }
    }

    res.placed += res.macrosPlaced;
    res.multiRowPlaced += res.macrosPlaced;
    res.unplaced = res.movable - res.placed;
    for (std::uint32_t v = 0; v < n; ++v) {
        const multirow::Cell &c = design.cells[v];
        if (!c.isFixed && !c.isTerminal) {
            const double moved = std::hypot(xs[v] - c.x, ys[v] - c.y);
            res.displacement += moved;
            res.maxDisplacement = std::max(res.maxDisplacement, moved);
        }
    }
    res.hpwlAfter = multirow::netlistHPWL(design.cells, design.nets, design.pins, xs, ys);
    res.seconds = timer.elapsedSeconds();
    return res;
}

MultiRowLegalizer::MultiRowLegalizer(const multirow::Design &design)
    : pImpl(std::make_unique<Impl>(design)) {}

MultiRowLegalizer::~MultiRowLegalizer() = default;

MultiRowLegalizer::MultiRowLegalizer(MultiRowLegalizer &&) noexcept = default;

MultiRowLegalizer &MultiRowLegalizer::operator=(MultiRowLegalizer &&) noexcept = default;

MultiRowLegalizeResult MultiRowLegalizer::legalize(const MultiRowOptions &options) {
    return pImpl->run(options);
}

multiRowSolution MultiRowLegalizer::solution() const {
    return {pImpl->xs, pImpl->ys};
}

}  // namespace ktplace
