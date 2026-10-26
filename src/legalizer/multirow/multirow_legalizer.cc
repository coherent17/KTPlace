#include "legalizer/multirow/multirow_legalizer.h"

#include "datamodel/kt_hpwl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace ktplace::multirow {

namespace {

constexpr double eps = 1e-9;
constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

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
    int region = noRegion;
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
    std::uint32_t root = none;
    std::vector<Extent> extents;

    [[nodiscard]] double getIdeal() const {
        return n > 0.0 ? -a / n : 0.0;
    }
    [[nodiscard]] const Extent *getIn(std::uint32_t lane) const {
        for (const Extent &e : extents) {
            if (e.lane == lane) {
                return &e;
            }
        }
        return nullptr;
    }
    [[nodiscard]] Extent *getIn(std::uint32_t lane) {
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

struct Stats {
    std::size_t movable = 0;
    std::size_t placed = 0;
    std::size_t unplaced = 0;
    std::size_t multiRow = 0;
    std::size_t multiRowPlaced = 0;
    std::size_t macros = 0;
    std::size_t macrosPlaced = 0;
    double displacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

void report(const Stats &res) {
    ktReportTable table("Legalization (multi-row)");
    table.setHeaders({"metric", "value"});
    table.addRow({"movable cells", std::format("{}", res.movable)});
    table.addRow({"cells placed", std::format("{}", res.placed)});
    table.addRow({"cells unplaced", std::format("{}", res.unplaced)});
    table.addRow({"cells taller than a row", std::format("{}", res.multiRow)});
    table.addRow({"of those, placed", std::format("{}", res.multiRowPlaced)});
    table.addRow({"macros legalized first", std::format("{}", res.macros)});
    table.addRow({"macros placed", std::format("{}", res.macrosPlaced)});
    table.addRow({"total displacement", std::format("{:.6e}", res.displacement)});
    table.addRow({"max displacement", std::format("{:.1f}", res.maxDisplacement)});
    table.addRow({"HPWL before", std::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", std::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"seconds", std::format("{:.3f}", res.seconds)});
    table.emit();
    if (res.unplaced > 0) {
        ktlog.warning("{} cell(s) found no legal position and kept their global placement",
                      res.unplaced);
    }
}

}  // namespace

class MultiRowLegalizer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    Stats run(const MultiRowOptions &params);

    std::vector<double> xs;
    std::vector<double> ys;

private:
    void legalizeMacros(const std::vector<std::uint32_t> &macros, Stats &res);
    void buildLanes();
    [[nodiscard]] std::vector<Slot> getSlots(std::size_t bottom, std::size_t span, int region,
                                             double width) const;
    [[nodiscard]] std::size_t getSpanOf(double height) const;
    [[nodiscard]] std::vector<std::uint32_t> fillGaps(std::vector<std::uint32_t> pending,
                                                      const std::vector<std::uint32_t> &placed,
                                                      std::vector<std::size_t> &bottomRow);

    [[nodiscard]] double getOffset(std::uint32_t cell) const;
    [[nodiscard]] double place(const Cluster &c, bool &feasible) const;
    [[nodiscard]] std::size_t getIndexIn(std::uint32_t lane, std::uint32_t cluster) const;
    std::uint32_t insert(std::uint32_t cell, const Slot &slot, double target, bool &feasible);
    void merge(std::uint32_t left, std::uint32_t right);

    void touchCluster(std::uint32_t id);
    void touchLane(std::uint32_t id);
    void setParent(std::uint32_t cell, std::uint32_t parent, double delta);
    void rollback();
    void forget();
    void resolvePlaced(const std::vector<std::uint32_t> &placed,
                       const std::vector<std::size_t> &bottomRow, std::vector<double> &px,
                       std::vector<double> &py) const;
    void writeFrame(const std::vector<double> &px, const std::vector<double> &py,
                    const std::string &note, std::size_t step, std::size_t total) const;
    [[nodiscard]] double getHpwlOf(const std::vector<double> &px,
                                   const std::vector<double> &py) const;

    Design &design;
    double hpwlInput = 0.0;
    double rowHeight = 0.0;
    const std::vector<RowInfo> &rows = design.rows;
    std::vector<BBox> blocked = design.blockages;
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
        const Cell &c = design.cells[v];
        const std::size_t span = getSpanOf(c.height);
        for (std::size_t k = bottomRow[v]; k < bottomRow[v] + span && k < rows.size(); ++k) {
            for (const std::uint32_t l : lanesOfRow[k]) {
                if (lanes[l].xlo <= xs[v] + eps && xs[v] + c.width <= lanes[l].xhi + eps) {
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
        const Cell &ca = design.cells[a];
        const Cell &cb = design.cells[b];
        return ca.width * ca.height > cb.width * cb.height;
    });
    std::vector<std::uint32_t> filled;
    std::vector<std::size_t> byDistance(rows.size());
    for (const std::uint32_t v : pending) {
        const Cell &cell = design.cells[v];
        const std::size_t span = getSpanOf(cell.height);
        const int region = cell.region;
        std::iota(byDistance.begin(), byDistance.end(), 0u);
        std::stable_sort(byDistance.begin(), byDistance.end(), [&](std::size_t a, std::size_t b) {
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
            for (const Slot &slot : getSlots(r, span, region, cell.width)) {
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
                    if (hi - lo < cell.width - eps) {
                        continue;
                    }
                    double x = std::clamp(cell.x, lo, hi - cell.width);
                    if (pitch > 0.0) {
                        x = std::round(x / pitch) * pitch;
                        if (x + cell.width > hi + eps) {
                            x -= pitch;
                        }
                        if (x < lo - eps) {
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

std::size_t MultiRowLegalizer::Impl::getSpanOf(double height) const {
    return static_cast<std::size_t>(std::max(1.0, std::ceil(height / rowHeight - 1e-6)));
}

void MultiRowLegalizer::Impl::legalizeMacros(const std::vector<std::uint32_t> &macros, Stats &res) {
    std::vector<std::uint32_t> order = macros;
    std::stable_sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        const Cell &ca = design.cells[a];
        const Cell &cb = design.cells[b];
        return ca.width * ca.height > cb.width * cb.height;
    });
    std::vector<std::size_t> byDistance(rows.size());
    for (const std::uint32_t v : order) {
        const Cell &cell = design.cells[v];
        std::iota(byDistance.begin(), byDistance.end(), 0u);
        std::stable_sort(byDistance.begin(), byDistance.end(), [&](std::size_t a, std::size_t b) {
            return std::abs(rows[a].coordinate - cell.y) < std::abs(rows[b].coordinate - cell.y);
        });
        const std::size_t span = getSpanOf(cell.height);
        double bestCost = std::numeric_limits<double>::max();
        BBox best;
        for (const std::size_t r : byDistance) {
            const double dy = rows[r].coordinate - cell.y;
            if (dy * dy >= bestCost) {
                break;
            }
            if (r + span > rows.size()) {
                continue;
            }
            Intervals free;
            for (const Subrow &s : rows[r].subrows) {
                free.emplace_back(s.xlo, s.xhi);
            }
            std::stable_sort(free.begin(), free.end());
            bool contiguous = true;
            for (std::size_t k = r + 1; k < r + span && contiguous; ++k) {
                contiguous = std::abs(rows[k].coordinate -
                                      (rows[k - 1].coordinate + rows[k - 1].height)) < 1e-6;
                Intervals here;
                for (const Subrow &s : rows[k].subrows) {
                    here.emplace_back(s.xlo, s.xhi);
                }
                std::stable_sort(here.begin(), here.end());
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
            for (const BBox &b : blocked) {
                if (b.yhi > y0 + eps && b.ylo < y1 - eps) {
                    free = subtract(free, b.xlo, b.xhi);
                }
            }
            const double pitch = rows[r].getPitch();
            for (const auto &[lo, hi] : free) {
                if (hi - lo < cell.width - eps) {
                    continue;
                }
                double x = std::clamp(cell.x, lo, hi - cell.width);
                if (pitch > 0.0) {
                    x = lo + std::round((x - lo) / pitch) * pitch;
                    if (x + cell.width > hi + eps) {
                        x -= pitch;
                    }
                    if (x < lo - eps) {
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
        xs[v] = best.xlo;
        ys[v] = best.ylo;
        blocked.push_back(best);
        ++res.macrosPlaced;
    }
}

void MultiRowLegalizer::Impl::buildLanes() {
    lanesOfRow.assign(rows.size(), {});
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const RowInfo &row = rows[r];
        const double y0 = row.coordinate;
        const double y1 = y0 + row.height;
        std::vector<std::tuple<double, double, int>> pieces;
        for (const Subrow &s : row.subrows) {
            if (!(s.xhi > s.xlo)) {
                continue;
            }
            Intervals free{{s.xlo, s.xhi}};
            for (const BBox &b : blocked) {
                if (b.yhi > y0 + eps && b.ylo < y1 - eps) {
                    free = subtract(free, b.xlo, b.xhi);
                }
            }
            for (const auto &[lo, hi] : free) {
                if (hi - lo >= std::max(row.getPitch(), eps)) {
                    pieces.emplace_back(lo, hi, s.region);
                }
            }
        }
        std::ranges::stable_sort(pieces);
        for (const auto &[lo, hi, region] : pieces) {
            lanesOfRow[r].push_back(static_cast<std::uint32_t>(lanes.size()));
            lanes.push_back({r, y0, row.height, lo, hi, row.getPitch(), region, {}});
        }
    }
}

std::vector<Slot> MultiRowLegalizer::Impl::getSlots(std::size_t bottom, std::size_t span,
                                                    int region, double width) const {
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
                if (lane.region == region && hi - lo >= width - eps) {
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
                                 return s.hi - s.lo < width - eps;
                             }),
              out.end());
    return out;
}

double MultiRowLegalizer::Impl::getOffset(std::uint32_t cell) const {
    double sum = 0.0;
    for (std::uint32_t c = cell; c != none; c = parent[c]) {
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
    feasible = lo <= hi + eps;
    double x = std::clamp(c.getIdeal(), lo, std::max(lo, hi));
    if (pitch > 0.0) {
        x = std::round(x / pitch) * pitch;
        if (x > hi + eps) {
            x -= pitch;
        }
        if (x < lo - eps) {
            x += pitch;
        }
        feasible = feasible && x >= lo - eps && x <= hi + eps;
    }
    return x;
}

std::size_t MultiRowLegalizer::Impl::getIndexIn(std::uint32_t lane, std::uint32_t cluster) const {
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
    for (auto &savedParent : std::ranges::reverse_view(savedParents)) {
        parent[std::get<0>(savedParent)] = std::get<1>(savedParent);
        delta[std::get<0>(savedParent)] = std::get<2>(savedParent);
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
        if (const Extent *mine = l.getIn(e.lane)) {
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
        const std::size_t at = getIndexIn(e.lane, right);
        if (Extent *mine = l.getIn(e.lane)) {
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
    const Cell &c = design.cells[cell];
    Cluster fresh;
    fresh.n = 1.0;
    fresh.a = -target;
    fresh.b = target * target;
    fresh.root = cell;
    for (const std::uint32_t l : slot.lanes) {
        fresh.extents.push_back({l, 0.0, c.width});
    }
    auto id = static_cast<std::uint32_t>(clusters.size());
    clusters.push_back(std::move(fresh));
    clusterTouched.push_back(0);
    setParent(cell, none, 0.0);
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
        std::uint32_t neighbour = none;
        bool neighbourIsLeft = false;
        for (const Extent &e : clusters[id].extents) {
            const std::vector<std::uint32_t> &list = lanes[e.lane].clusters;
            const std::size_t at = getIndexIn(e.lane, id);
            if (at > 0) {
                const Cluster &p = clusters[list[at - 1]];
                const Extent *pe = p.getIn(e.lane);
                if (p.x + pe->hi > x + e.lo + eps) {
                    neighbour = list[at - 1];
                    neighbourIsLeft = true;
                    break;
                }
            }
            if (at + 1 < list.size()) {
                const Cluster &s = clusters[list[at + 1]];
                const Extent *se = s.getIn(e.lane);
                if (x + e.hi > s.x + se->lo + eps) {
                    neighbour = list[at + 1];
                    neighbourIsLeft = false;
                    break;
                }
            }
        }
        if (neighbour == none) {
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

Stats MultiRowLegalizer::Impl::run(const MultiRowOptions &params) {
    ScopedTimer timer("multirow");
    Stats res;

    rowHeight = std::numeric_limits<double>::max();
    for (const RowInfo &r : rows) {
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
        const Cell &c = design.cells[v];
        xs[v] = c.x;
        ys[v] = c.y;
        if (!c.isMovable) {
            continue;
        }
        ++res.movable;
        if (c.height > 1.5 * rowHeight) {
            ++res.multiRow;
        }
        if (getSpanOf(c.height) > params.macroRows) {
            macros.push_back(v);
        } else {
            cells.push_back(v);
        }
    }
    res.macros = macros.size();
    res.hpwlBefore = getHpwlOf(xs, ys);
    hpwlInput = res.hpwlBefore;
    const std::size_t total = cells.size() + macros.size();
    writeFrame(xs, ys, "Input - global placement", 0, total);
    if (rows.empty()) {
        res.unplaced = res.movable;
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }

    legalizeMacros(macros, res);
    if (!macros.empty()) {
        writeFrame(xs, ys, "Macros placed", macros.size(), total);
    }
    buildLanes();

    parent.assign(n, none);
    delta.assign(n, 0.0);
    clusterOf.assign(n, none);
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
    const std::size_t frameInterval = design.onFrame && params.frames > 0
                                          ? (cells.size() + params.frames - 1) / params.frames
                                          : 0;
    for (const std::uint32_t v : cells) {
        const Cell &cell = design.cells[v];
        const std::size_t span = getSpanOf(cell.height);
        const int region = cell.region;
        const std::size_t home = static_cast<std::size_t>(
            std::lower_bound(rowY.begin(), rowY.end(), cell.y) - rowY.begin());

        double bestCost = std::numeric_limits<double>::max();
        Slot best;
        std::size_t bestRow = 0;
        const auto tryRow = [&](std::size_t r) {
            const double dy = rows[r].coordinate - cell.y;
            for (const Slot &slot : getSlots(r, span, region, cell.width)) {
                const double dx = cell.x < slot.lo                ? slot.lo - cell.x
                                  : cell.x > slot.hi - cell.width ? cell.x - (slot.hi - cell.width)
                                                                  : 0.0;
                if (dx * dx + dy * dy >= bestCost) {
                    continue;
                }
                bool feasible = false;
                const std::uint32_t id = insert(v, slot, cell.x, feasible);
                if (feasible) {
                    const double moved = clusters[id].x + getOffset(v) - cell.x;
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
        if (frameInterval > 0 && placedCells.size() % frameInterval == 0) {
            std::vector<double> px = xs;
            std::vector<double> py = ys;
            resolvePlaced(placedCells, bottomRow, px, py);
            writeFrame(px, py, "Placing cells into rows", macros.size() + placedCells.size(),
                       total);
        }
    }

    resolvePlaced(placedCells, bottomRow, xs, ys);

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
        const Cell &c = design.cells[v];
        if (c.isMovable) {
            const double moved =
                std::sqrt((xs[v] - c.x) * (xs[v] - c.x) + (ys[v] - c.y) * (ys[v] - c.y));
            res.displacement += moved;
            res.maxDisplacement = std::max(res.maxDisplacement, moved);
        }
    }
    res.hpwlAfter = getHpwlOf(xs, ys);
    writeFrame(xs, ys, "Legal placement", res.placed, total);
    for (std::uint32_t v = 0; v < n; ++v) {
        design.cells[v].x = xs[v];
        design.cells[v].y = ys[v];
    }
    res.seconds = timer.getElapsedSeconds();
    return res;
}

void MultiRowLegalizer::Impl::resolvePlaced(const std::vector<std::uint32_t> &placed,
                                            const std::vector<std::size_t> &bottomRow,
                                            std::vector<double> &px,
                                            std::vector<double> &py) const {
    std::vector<double> clusterX(px.size(), 0.0);
    for (const Cluster &c : clusters) {
        if (c.n > 0.0) {
            clusterX[c.root] = c.x;
        }
    }
    for (const std::uint32_t v : placed) {
        std::uint32_t root = v;
        double sum = 0.0;
        for (std::uint32_t c = v; c != none; c = parent[c]) {
            sum += delta[c];
            root = c;
        }
        px[v] = clusterX[root] + sum;
        py[v] = rows[bottomRow[v]].coordinate;
    }
}

void MultiRowLegalizer::Impl::writeFrame(const std::vector<double> &px,
                                         const std::vector<double> &py, const std::string &note,
                                         std::size_t step, std::size_t total) const {
    if (!design.onFrame) {
        return;
    }
    const std::vector<float> fx(px.begin(), px.end());
    const std::vector<float> fy(py.begin(), py.end());
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.numSteps = total;
    info.hpwl = getHpwlOf(px, py);
    info.hpwlInitial = hpwlInput;
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

double MultiRowLegalizer::Impl::getHpwlOf(const std::vector<double> &px,
                                          const std::vector<double> &py) const {
    return getNetHpwl(
        design.nets.size(),
        [this](std::size_t n) {
            return design.nets[n];
        },
        [&](const NetPin &pin) {
            return std::pair{px[pin.cellId] + pin.offX, py[pin.cellId] + pin.offY};
        });
}

MultiRowLegalizer::MultiRowLegalizer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

MultiRowLegalizer::~MultiRowLegalizer() = default;

MultiRowLegalizer::MultiRowLegalizer(MultiRowLegalizer &&) noexcept = default;

MultiRowLegalizer &MultiRowLegalizer::operator=(MultiRowLegalizer &&) noexcept = default;

void MultiRowLegalizer::legalize(const MultiRowOptions &options) {
    report(pImpl->run(options));
}

}  // namespace ktplace::multirow
