#include "detailPlacer/abcdplace/abcdplace_placer.h"

#include "datamodel/kt_hpwl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <span>
#include <string>
#include <tuple>
#include <vector>

namespace ktplace::abcdplace {

namespace {

constexpr double inf = std::numeric_limits<double>::infinity();
constexpr std::size_t noCell = std::numeric_limits<std::size_t>::max();
constexpr double gain = 1e-6;

struct Segment {
    Interval x;
    double y = 0.0;
    std::size_t row = 0;
    int region = noRegion;
    double site = 1.0;
    std::vector<std::size_t> cells;
};

struct Move {
    std::size_t cell = noCell;
    Point to;
};

struct Candidate {
    double delta = 0.0;
    std::size_t other = noCell;
    Point to;
    bool intoSpace = false;
};

struct Step {
    std::string name;
    double hpwl = 0.0;
    double seconds = 0.0;
    std::size_t moves = 0;
};

std::uint64_t getNextRandom(std::uint64_t &state) {
    state += 0x9e3779b97f4a7c15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31U);
}

template <typename Body>
void parallelFor(std::size_t n, std::size_t grain, Body body) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, grain),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t i = range.begin(); i < range.end(); ++i) {
                              body(i);
                          }
                      });
}

void report(const std::vector<Step> &steps, double initial) {
    ktReportTable t("Detailed placement (ABCDPlace)");
    t.setHeaders({"step", "HPWL", "change", "moves", "seconds"});
    t.addRow({"input", std::format("{:.6e}", initial), "", "", ""});
    double before = initial;
    for (const Step &s : steps) {
        t.addRow(
            {s.name, std::format("{:.6e}", s.hpwl),
             std::format("{:+.3f}%", 100.0 * (s.hpwl - before) / (before > 0.0 ? before : 1.0)),
             std::format("{}", s.moves), std::format("{:.3f}", s.seconds)});
        before = s.hpwl;
    }
    t.addRow({"total", std::format("{:.6e}", before),
              std::format("{:+.3f}%", 100.0 * (before - initial) / (initial > 0.0 ? initial : 1.0)),
              "", ""});
    t.emit();
}

}  // namespace

std::vector<std::size_t> solveAssignment(const std::vector<double> &cost, std::size_t n) {
    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(n + 1, 0.0);
    std::vector<std::size_t> column(n + 1, 0);
    std::vector<std::size_t> way(n + 1, 0);
    for (std::size_t row = 1; row <= n; ++row) {
        column[0] = row;
        std::size_t j0 = 0;
        std::vector<double> least(n + 1, inf);
        std::vector<char> used(n + 1, 0);
        do {
            used[j0] = 1;
            const std::size_t i0 = column[j0];
            double delta = inf;
            std::size_t j1 = 0;
            for (std::size_t j = 1; j <= n; ++j) {
                if (used[j] != 0) {
                    continue;
                }
                const double reduced = cost[(i0 - 1) * n + (j - 1)] - u[i0] - v[j];
                if (reduced < least[j]) {
                    least[j] = reduced;
                    way[j] = j0;
                }
                if (least[j] < delta) {
                    delta = least[j];
                    j1 = j;
                }
            }
            for (std::size_t j = 0; j <= n; ++j) {
                if (used[j] != 0) {
                    u[column[j]] += delta;
                    v[j] -= delta;
                } else {
                    least[j] -= delta;
                }
            }
            j0 = j1;
        } while (column[j0] != 0);
        do {
            const std::size_t j1 = way[j0];
            column[j0] = column[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    std::vector<std::size_t> assigned(n, 0);
    for (std::size_t j = 1; j <= n; ++j) {
        assigned[column[j] - 1] = j - 1;
    }
    return assigned;
}

class AbcdPlacer::Impl {
public:
    explicit Impl(Design &design) : d(design) {}

    void place(const AbcdplaceOptions &options);

private:
    void buildSegments();
    void assignCells();
    void indexNets();
    [[nodiscard]] Point getPosition(std::size_t c) const;
    [[nodiscard]] double getNetHpwl(std::size_t n, std::span<const Move> moves) const;
    [[nodiscard]] double getDelta(std::span<const Move> moves) const;
    [[nodiscard]] double getHpwl() const;
    [[nodiscard]] double snapUp(double x, std::size_t s) const;
    [[nodiscard]] double getRightLimit(std::size_t c) const;

    std::size_t reorderSegment(std::size_t s);
    [[nodiscard]] std::vector<std::vector<std::size_t>> getIndependentRowGroups() const;
    std::size_t reorderLocally();

    [[nodiscard]] std::vector<std::vector<std::size_t>> getIndependentSubsets(
        std::size_t iteration) const;
    std::size_t matchIndependentSets(std::size_t iteration);

    [[nodiscard]] Point getOptimalPoint(std::size_t c) const;
    [[nodiscard]] std::size_t getBin(const Point &p) const;
    [[nodiscard]] bool canSwap(std::size_t i, std::size_t j) const;
    [[nodiscard]] bool canMoveAfter(std::size_t i, std::size_t j, double x) const;
    [[nodiscard]] Candidate getBestSwap(std::size_t i) const;
    void applySwap(std::size_t i, std::size_t j);
    void applyMoveAfter(std::size_t i, std::size_t j, double x);
    void rebin(std::size_t c, std::size_t from);
    std::size_t swapGlobally();

    void writeFrame(const std::string &note, std::size_t step, double hpwlInitial) const;

    Design &d;
    AbcdplaceOptions opts;
    std::vector<Segment> segments;
    std::vector<std::vector<std::size_t>> segmentsInRow;
    std::vector<double> rowY;
    double rowHeight = 1.0;
    std::vector<std::size_t> segmentOf;
    std::vector<std::size_t> indexInSegment;
    std::vector<std::vector<std::size_t>> cellNets;
    std::vector<BBox> blocked;
    std::size_t binsX = 1;
    std::size_t binsY = 1;
    double binSize = 1.0;
    std::vector<std::vector<std::size_t>> binCells;
    std::vector<std::size_t> binOf;
};

Point AbcdPlacer::Impl::getPosition(std::size_t c) const {
    return Point(d.cells.x[c], d.cells.y[c]);
}

double AbcdPlacer::Impl::getNetHpwl(std::size_t n, std::span<const Move> moves) const {
    const auto pins = d.nets[n];
    if (pins.size() < 2) {
        return 0.0;
    }
    BBox box = BBox::getEmpty();
    for (const NetPin &pin : pins) {
        if (pin.slot == noSlot) {
            box.extend(pin.fixedPosition.x, pin.fixedPosition.y);
            continue;
        }
        Point at = getPosition(pin.slot);
        for (const Move &m : moves) {
            if (m.cell == pin.slot) {
                at = m.to;
                break;
            }
        }
        box.extend(at.x + pin.offset.x, at.y + pin.offset.y);
    }
    return box.getHalfPerimeter();
}

double AbcdPlacer::Impl::getDelta(std::span<const Move> moves) const {
    std::vector<std::size_t> nets;
    for (const Move &m : moves) {
        nets.insert(nets.end(), cellNets[m.cell].begin(), cellNets[m.cell].end());
    }
    std::ranges::sort(nets);
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
    double delta = 0.0;
    for (const std::size_t n : nets) {
        delta += getNetHpwl(n, moves) - getNetHpwl(n, {});
    }
    return delta;
}

double AbcdPlacer::Impl::getHpwl() const {
    return ktplace::getNetHpwl(
        d.nets.size(),
        [this](std::size_t n) {
            return d.nets[n];
        },
        [this](const NetPin &pin) {
            return pin.slot == noSlot ? pin.fixedPosition
                                      : Point(d.cells.x[pin.slot] + pin.offset.x,
                                              d.cells.y[pin.slot] + pin.offset.y);
        });
}

double AbcdPlacer::Impl::snapUp(double x, std::size_t s) const {
    const double site = segments[s].site;
    return std::ceil(x / site - eps) * site;
}

double AbcdPlacer::Impl::getRightLimit(std::size_t c) const {
    const Segment &seg = segments[segmentOf[c]];
    const std::size_t k = indexInSegment[c];
    return k + 1 < seg.cells.size() ? d.cells.x[seg.cells[k + 1]] : seg.x.end;
}

void AbcdPlacer::Impl::buildSegments() {
    const std::vector<RowInfo> &rows = d.rows;
    rowY.clear();
    rowHeight = inf;
    for (const RowInfo &row : rows) {
        rowY.push_back(row.coordinate);
        if (row.height > 0.0) {
            rowHeight = std::min(rowHeight, row.height);
        }
    }
    if (!std::isfinite(rowHeight)) {
        rowHeight = 1.0;
    }
    std::vector<std::vector<Interval>> obstacles(rows.size());
    for (const BBox &box : blocked) {
        auto r = std::ranges::upper_bound(rows, box.lowerLeft.y + eps, {}, [](const RowInfo &row) {
            return row.coordinate + row.height;
        });
        for (; r != rows.end() && r->coordinate < box.upperRight.y - eps; ++r) {
            obstacles[static_cast<std::size_t>(r - rows.begin())].push_back(box.getX());
        }
    }
    segments.clear();
    segmentsInRow.assign(rows.size(), {});
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const RowInfo &row = rows[r];
        std::vector<Interval> &obs = obstacles[r];
        std::ranges::stable_sort(obs);
        for (const Subrow &sub : row.subrows) {
            const auto emit = [&](double lo, double hi) {
                if (hi > lo + eps) {
                    segmentsInRow[r].push_back(segments.size());
                    segments.push_back(Segment{.x = Interval(lo, hi),
                                               .y = row.coordinate,
                                               .row = r,
                                               .region = sub.region,
                                               .site = row.getPitch() > 0.0 ? row.getPitch() : 1.0,
                                               .cells = {}});
                }
            };
            double cursor = sub.x.start;
            for (const Interval &o : obs) {
                if (o.end <= cursor + eps || o.start >= sub.x.end - eps) {
                    continue;
                }
                emit(cursor, std::min(o.start, sub.x.end));
                cursor = std::max(cursor, o.end);
            }
            emit(cursor, sub.x.end);
        }
    }
}

void AbcdPlacer::Impl::assignCells() {
    const Cells &cells = d.cells;
    segmentOf.assign(cells.size(), noCell);
    indexInSegment.assign(cells.size(), noCell);
    for (std::size_t c = 0; c < cells.size(); ++c) {
        const auto row = std::ranges::lower_bound(rowY, cells.y[c] - eps);
        if (row == rowY.end() || std::abs(*row - cells.y[c]) > eps) {
            continue;
        }
        for (const std::size_t s : segmentsInRow[static_cast<std::size_t>(row - rowY.begin())]) {
            const Segment &seg = segments[s];
            if (seg.region == cells.region[c] && cells.x[c] >= seg.x.start - eps &&
                cells.x[c] + cells.width[c] <= seg.x.end + eps) {
                segmentOf[c] = s;
                segments[s].cells.push_back(c);
                break;
            }
        }
    }
    for (Segment &seg : segments) {
        std::ranges::stable_sort(seg.cells, {}, [&](std::size_t c) {
            return cells.x[c];
        });
        for (std::size_t k = 0; k < seg.cells.size(); ++k) {
            indexInSegment[seg.cells[k]] = k;
        }
    }
}

void AbcdPlacer::Impl::indexNets() {
    cellNets.assign(d.cells.size(), {});
    for (std::size_t n = 0; n < d.nets.size(); ++n) {
        for (const NetPin &pin : d.nets[n]) {
            if (pin.slot != noSlot &&
                (cellNets[pin.slot].empty() || cellNets[pin.slot].back() != n)) {
                cellNets[pin.slot].push_back(n);
            }
        }
    }
}

std::size_t AbcdPlacer::Impl::reorderSegment(std::size_t s) {
    Segment &seg = segments[s];
    std::vector<double> &xs = d.cells.x;
    const std::vector<double> &ws = d.cells.width;
    std::size_t moves = 0;
    struct NetView {
        double outsideLo = inf;
        double outsideHi = -inf;
        double ySpan = 0.0;
        std::vector<std::pair<std::size_t, double>> pins;
        bool counted = false;
    };
    std::vector<std::size_t> nets;
    std::vector<NetView> views;
    std::vector<std::size_t> order;
    std::vector<std::size_t> best;
    std::vector<double> placed;
    for (std::size_t start = 0; start + 1 < seg.cells.size(); ++start) {
        const std::size_t m = std::min(opts.reorderWindow, seg.cells.size() - start);
        const std::span<const std::size_t> window(seg.cells.data() + start, m);
        nets.clear();
        for (const std::size_t c : window) {
            nets.insert(nets.end(), cellNets[c].begin(), cellNets[c].end());
        }
        std::ranges::sort(nets);
        nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
        views.assign(nets.size(), NetView{});
        for (std::size_t k = 0; k < nets.size(); ++k) {
            NetView &view = views[k];
            const auto pins = d.nets[nets[k]];
            view.counted = pins.size() >= 2;
            double yLo = inf;
            double yHi = -inf;
            for (const NetPin &pin : pins) {
                double px = 0.0;
                double py = 0.0;
                if (pin.slot == noSlot) {
                    px = pin.fixedPosition.x;
                    py = pin.fixedPosition.y;
                } else {
                    py = d.cells.y[pin.slot] + pin.offset.y;
                    const auto inWindow = std::ranges::find(window, pin.slot);
                    if (inWindow != window.end()) {
                        view.pins.emplace_back(static_cast<std::size_t>(inWindow - window.begin()),
                                               pin.offset.x);
                        yLo = std::min(yLo, py);
                        yHi = std::max(yHi, py);
                        continue;
                    }
                    px = xs[pin.slot] + pin.offset.x;
                }
                view.outsideLo = std::min(view.outsideLo, px);
                view.outsideHi = std::max(view.outsideHi, px);
                yLo = std::min(yLo, py);
                yHi = std::max(yHi, py);
            }
            view.ySpan = yHi - yLo;
        }
        const double left = xs[window.front()];
        const auto getCost = [&](const std::vector<double> &x) {
            double total = 0.0;
            for (const NetView &view : views) {
                if (!view.counted) {
                    continue;
                }
                double lo = view.outsideLo;
                double hi = view.outsideHi;
                for (const auto &[k, offset] : view.pins) {
                    lo = std::min(lo, x[k] + offset);
                    hi = std::max(hi, x[k] + offset);
                }
                total += (hi - lo) + view.ySpan;
            }
            return total;
        };
        placed.assign(m, 0.0);
        for (std::size_t k = 0; k < m; ++k) {
            placed[k] = xs[window[k]];
        }
        const double current = getCost(placed);
        double bestCost = current;
        order.resize(m);
        std::iota(order.begin(), order.end(), std::size_t{0});
        best.clear();
        do {
            double cursor = left;
            for (const std::size_t k : order) {
                placed[k] = cursor;
                cursor += ws[window[k]];
            }
            const double cost = getCost(placed);
            if (cost < bestCost - gain) {
                bestCost = cost;
                best = order;
            }
        } while (std::next_permutation(order.begin(), order.end()));
        if (best.empty()) {
            continue;
        }
        std::vector<std::size_t> cells(window.begin(), window.end());
        double cursor = left;
        for (std::size_t k = 0; k < m; ++k) {
            const std::size_t c = cells[best[k]];
            xs[c] = cursor;
            cursor += ws[c];
            seg.cells[start + k] = c;
            indexInSegment[c] = start + k;
        }
        ++moves;
    }
    return moves;
}

std::vector<std::vector<std::size_t>> AbcdPlacer::Impl::getIndependentRowGroups() const {
    const std::size_t numRows = segmentsInRow.size();
    std::vector<std::vector<std::size_t>> rowNets(numRows);
    for (std::size_t c = 0; c < d.cells.size(); ++c) {
        if (segmentOf[c] == noCell) {
            continue;
        }
        std::vector<std::size_t> &nets = rowNets[segments[segmentOf[c]].row];
        nets.insert(nets.end(), cellNets[c].begin(), cellNets[c].end());
    }
    std::vector<std::vector<std::size_t>> netColors(d.nets.size());
    std::vector<std::size_t> colorOf(numRows, 0);
    std::vector<char> forbidden;
    std::size_t colors = 0;
    for (std::size_t r = 0; r < numRows; ++r) {
        std::vector<std::size_t> &nets = rowNets[r];
        std::ranges::sort(nets);
        nets.erase(std::unique(nets.begin(), nets.end()), nets.end());
        forbidden.assign(colors + 1, 0);
        for (const std::size_t n : nets) {
            for (const std::size_t color : netColors[n]) {
                forbidden[color] = 1;
            }
        }
        std::size_t color = 0;
        while (forbidden[color] != 0) {
            ++color;
        }
        colorOf[r] = color;
        colors = std::max(colors, color + 1);
        for (const std::size_t n : nets) {
            if (std::ranges::find(netColors[n], color) == netColors[n].end()) {
                netColors[n].push_back(color);
            }
        }
    }
    std::vector<std::vector<std::size_t>> groups(colors);
    for (std::size_t r = 0; r < numRows; ++r) {
        groups[colorOf[r]].push_back(r);
    }
    return groups;
}

std::size_t AbcdPlacer::Impl::reorderLocally() {
    if (opts.reorderWindow < 2) {
        return 0;
    }
    std::vector<std::size_t> movesInRow(segmentsInRow.size(), 0);
    for (const std::vector<std::size_t> &group : getIndependentRowGroups()) {
        parallelFor(group.size(), 1, [&](std::size_t k) {
            const std::size_t r = group[k];
            for (const std::size_t s : segmentsInRow[r]) {
                movesInRow[r] += reorderSegment(s);
            }
        });
    }
    return std::accumulate(movesInRow.begin(), movesInRow.end(), std::size_t{0});
}

std::vector<std::vector<std::size_t>> AbcdPlacer::Impl::getIndependentSubsets(
    std::size_t iteration) const {
    const Cells &cells = d.cells;
    std::vector<std::size_t> order(cells.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::uint64_t state = opts.seed * 0x100000001b3ULL + iteration;
    for (std::size_t k = order.size(); k > 1; --k) {
        std::swap(order[k - 1], order[getNextRandom(state) % k]);
    }
    std::vector<char> netTaken(d.nets.size(), 0);
    std::map<std::tuple<double, double, int>, std::vector<std::size_t>> bySize;
    for (const std::size_t c : order) {
        if (segmentOf[c] == noCell || cellNets[c].empty() ||
            std::ranges::any_of(cellNets[c], [&](std::size_t n) {
                return netTaken[n] != 0;
            })) {
            continue;
        }
        for (const std::size_t n : cellNets[c]) {
            netTaken[n] = 1;
        }
        bySize[{cells.width[c], cells.height[c], cells.region[c]}].push_back(c);
    }

    const double bin = opts.ismBinRows * rowHeight;
    const auto nx =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(d.die.getWidth() / bin)));
    const auto ny =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(d.die.getHeight() / bin)));
    const auto binAt = [&](std::size_t c, std::size_t &bx, std::size_t &by) {
        bx = std::min(nx - 1, static_cast<std::size_t>(
                                  std::max(0.0, (cells.x[c] - d.die.lowerLeft.x) / bin)));
        by = std::min(ny - 1, static_cast<std::size_t>(
                                  std::max(0.0, (cells.y[c] - d.die.lowerLeft.y) / bin)));
    };
    std::vector<std::vector<std::size_t>> subsets;
    for (const auto &[size, group] : bySize) {
        if (group.size() < 2) {
            continue;
        }
        std::map<std::size_t, std::vector<std::size_t>> binned;
        for (std::size_t k = 0; k < group.size(); ++k) {
            std::size_t bx = 0;
            std::size_t by = 0;
            binAt(group[k], bx, by);
            binned[by * nx + bx].push_back(k);
        }
        std::vector<char> taken(group.size(), 0);
        for (std::size_t seed = 0; seed < group.size(); ++seed) {
            if (taken[seed] != 0) {
                continue;
            }
            std::size_t sx = 0;
            std::size_t sy = 0;
            binAt(group[seed], sx, sy);
            std::vector<std::size_t> subset;
            const auto visit = [&](long bx, long by) {
                if (bx < 0 || by < 0 || bx >= static_cast<long>(nx) ||
                    by >= static_cast<long>(ny)) {
                    return;
                }
                const auto found =
                    binned.find(static_cast<std::size_t>(by) * nx + static_cast<std::size_t>(bx));
                if (found == binned.end()) {
                    return;
                }
                for (const std::size_t k : found->second) {
                    if (subset.size() < opts.ismSetSize && taken[k] == 0) {
                        taken[k] = 1;
                        subset.push_back(group[k]);
                    }
                }
            };
            const auto cx = static_cast<long>(sx);
            const auto cy = static_cast<long>(sy);
            for (long ring = 0;
                 ring <= static_cast<long>(opts.ismSearchRings) && subset.size() < opts.ismSetSize;
                 ++ring) {
                if (ring == 0) {
                    visit(cx, cy);
                    continue;
                }
                for (long t = -ring; t <= ring; ++t) {
                    visit(cx + t, cy - ring);
                    visit(cx + t, cy + ring);
                }
                for (long t = -ring + 1; t <= ring - 1; ++t) {
                    visit(cx - ring, cy + t);
                    visit(cx + ring, cy + t);
                }
            }
            if (subset.size() >= 2) {
                subsets.push_back(std::move(subset));
            }
        }
    }
    return subsets;
}

std::size_t AbcdPlacer::Impl::matchIndependentSets(std::size_t iteration) {
    const std::vector<std::vector<std::size_t>> subsets = getIndependentSubsets(iteration);
    std::vector<std::vector<std::size_t>> assignment(subsets.size());
    parallelFor(subsets.size(), 1, [&](std::size_t s) {
        const std::vector<std::size_t> &subset = subsets[s];
        const std::size_t k = subset.size();
        std::vector<double> cost(k * k);
        for (std::size_t i = 0; i < k; ++i) {
            for (std::size_t j = 0; j < k; ++j) {
                const Move move{.cell = subset[i], .to = getPosition(subset[j])};
                double total = 0.0;
                for (const std::size_t n : cellNets[subset[i]]) {
                    total += getNetHpwl(n, std::span(&move, 1));
                }
                cost[i * k + j] = total;
            }
        }
        std::vector<std::size_t> solution = solveAssignment(cost, k);
        double before = 0.0;
        double after = 0.0;
        for (std::size_t i = 0; i < k; ++i) {
            before += cost[i * k + i];
            after += cost[i * k + solution[i]];
        }
        if (after < before - gain) {
            assignment[s] = std::move(solution);
        }
    });
    std::size_t moved = 0;
    for (std::size_t s = 0; s < subsets.size(); ++s) {
        if (assignment[s].empty()) {
            continue;
        }
        const std::vector<std::size_t> &subset = subsets[s];
        std::vector<Point> to;
        std::vector<std::size_t> toSegment;
        std::vector<std::size_t> toIndex;
        for (std::size_t i = 0; i < subset.size(); ++i) {
            const std::size_t target = subset[assignment[s][i]];
            to.push_back(getPosition(target));
            toSegment.push_back(segmentOf[target]);
            toIndex.push_back(indexInSegment[target]);
        }
        for (std::size_t i = 0; i < subset.size(); ++i) {
            const std::size_t c = subset[i];
            if (assignment[s][i] != i) {
                ++moved;
            }
            d.cells.x[c] = to[i].x;
            d.cells.y[c] = to[i].y;
            segmentOf[c] = toSegment[i];
            indexInSegment[c] = toIndex[i];
            segments[toSegment[i]].cells[toIndex[i]] = c;
        }
    }
    return moved;
}

Point AbcdPlacer::Impl::getOptimalPoint(std::size_t c) const {
    std::vector<double> xs;
    std::vector<double> ys;
    for (const std::size_t n : cellNets[c]) {
        BBox box = BBox::getEmpty();
        bool others = false;
        for (const NetPin &pin : d.nets[n]) {
            if (pin.slot == c) {
                continue;
            }
            const Point at = pin.slot == noSlot ? pin.fixedPosition
                                                : Point(d.cells.x[pin.slot] + pin.offset.x,
                                                        d.cells.y[pin.slot] + pin.offset.y);
            box.extend(at.x, at.y);
            others = true;
        }
        if (others) {
            xs.push_back(box.lowerLeft.x);
            xs.push_back(box.upperRight.x);
            ys.push_back(box.lowerLeft.y);
            ys.push_back(box.upperRight.y);
        }
    }
    if (xs.empty()) {
        return Point(d.cells.x[c] + d.cells.width[c] / 2.0, d.cells.y[c] + d.cells.height[c] / 2.0);
    }
    std::ranges::sort(xs);
    std::ranges::sort(ys);
    const std::size_t m = xs.size();
    return Point((xs[(m - 1) / 2] + xs[m / 2]) / 2.0, (ys[(m - 1) / 2] + ys[m / 2]) / 2.0);
}

std::size_t AbcdPlacer::Impl::getBin(const Point &p) const {
    const auto bx = std::min(
        binsX - 1, static_cast<std::size_t>(std::max(0.0, (p.x - d.die.lowerLeft.x) / binSize)));
    const auto by = std::min(
        binsY - 1, static_cast<std::size_t>(std::max(0.0, (p.y - d.die.lowerLeft.y) / binSize)));
    return by * binsX + bx;
}

bool AbcdPlacer::Impl::canSwap(std::size_t i, std::size_t j) const {
    if (segmentOf[i] == segmentOf[j] && (indexInSegment[i] + 1 == indexInSegment[j] ||
                                         indexInSegment[j] + 1 == indexInSegment[i])) {
        return false;
    }
    const Cells &cells = d.cells;
    return cells.width[i] <= getRightLimit(j) - cells.x[j] + eps &&
           cells.width[j] <= getRightLimit(i) - cells.x[i] + eps;
}

bool AbcdPlacer::Impl::canMoveAfter(std::size_t i, std::size_t j, double x) const {
    if (segmentOf[i] == segmentOf[j] && indexInSegment[j] + 1 == indexInSegment[i]) {
        return false;
    }
    const Cells &cells = d.cells;
    return x >= cells.x[j] + cells.width[j] - eps && x + cells.width[i] <= getRightLimit(j) + eps;
}

Candidate AbcdPlacer::Impl::getBestSwap(std::size_t i) const {
    Candidate best;
    if (segmentOf[i] == noCell) {
        return best;
    }
    const Cells &cells = d.cells;
    const Point optimal = getOptimalPoint(i);
    for (const std::size_t j : binCells[getBin(optimal)]) {
        if (j == i || segmentOf[j] == noCell || cells.region[j] != cells.region[i] ||
            cells.height[j] != cells.height[i]) {
            continue;
        }
        if (canSwap(i, j)) {
            const Move moves[2] = {Move{.cell = i, .to = getPosition(j)},
                                   Move{.cell = j, .to = getPosition(i)}};
            const double delta = getDelta(moves);
            if (delta < best.delta - gain) {
                best =
                    Candidate{.delta = delta, .other = j, .to = getPosition(j), .intoSpace = false};
            }
        }
        const std::size_t s = segmentOf[j];
        const double lo = snapUp(cells.x[j] + cells.width[j], s);
        const double hi = getRightLimit(j) - cells.width[i];
        if (hi + eps < lo) {
            continue;
        }
        const double site = segments[s].site;
        const double wanted = std::round((optimal.x - cells.width[i] / 2.0) / site) * site;
        const double x = std::clamp(wanted, lo, std::floor(hi / site + eps) * site);
        if (x + eps < lo || !canMoveAfter(i, j, x)) {
            continue;
        }
        const Move move{.cell = i, .to = Point(x, cells.y[j])};
        const double delta = getDelta(std::span(&move, 1));
        if (delta < best.delta - gain) {
            best = Candidate{.delta = delta, .other = j, .to = move.to, .intoSpace = true};
        }
    }
    return best;
}

void AbcdPlacer::Impl::rebin(std::size_t c, std::size_t from) {
    const std::size_t to = getBin(
        Point(d.cells.x[c] + d.cells.width[c] / 2.0, d.cells.y[c] + d.cells.height[c] / 2.0));
    if (to == from) {
        return;
    }
    std::vector<std::size_t> &old = binCells[from];
    old.erase(std::ranges::find(old, c));
    binCells[to].push_back(c);
    binOf[c] = to;
}

void AbcdPlacer::Impl::applySwap(std::size_t i, std::size_t j) {
    Cells &cells = d.cells;
    std::swap(cells.x[i], cells.x[j]);
    std::swap(cells.y[i], cells.y[j]);
    std::swap(segmentOf[i], segmentOf[j]);
    std::swap(indexInSegment[i], indexInSegment[j]);
    segments[segmentOf[i]].cells[indexInSegment[i]] = i;
    segments[segmentOf[j]].cells[indexInSegment[j]] = j;
    rebin(i, binOf[i]);
    rebin(j, binOf[j]);
}

void AbcdPlacer::Impl::applyMoveAfter(std::size_t i, std::size_t j, double x) {
    Segment &from = segments[segmentOf[i]];
    from.cells.erase(from.cells.begin() + static_cast<std::ptrdiff_t>(indexInSegment[i]));
    for (std::size_t k = indexInSegment[i]; k < from.cells.size(); ++k) {
        indexInSegment[from.cells[k]] = k;
    }
    const std::size_t s = segmentOf[j];
    Segment &into = segments[s];
    const std::size_t at = indexInSegment[j] + 1;
    into.cells.insert(into.cells.begin() + static_cast<std::ptrdiff_t>(at), i);
    for (std::size_t k = at; k < into.cells.size(); ++k) {
        indexInSegment[into.cells[k]] = k;
    }
    segmentOf[i] = s;
    d.cells.x[i] = x;
    d.cells.y[i] = into.y;
    rebin(i, binOf[i]);
}

std::size_t AbcdPlacer::Impl::swapGlobally() {
    const Cells &cells = d.cells;
    binSize = opts.swapBinRows * rowHeight;
    binsX =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(d.die.getWidth() / binSize)));
    binsY =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(d.die.getHeight() / binSize)));
    binCells.assign(binsX * binsY, {});
    binOf.assign(cells.size(), noCell);
    for (std::size_t c = 0; c < cells.size(); ++c) {
        if (segmentOf[c] != noCell) {
            binOf[c] = getBin(
                Point(cells.x[c] + cells.width[c] / 2.0, cells.y[c] + cells.height[c] / 2.0));
            binCells[binOf[c]].push_back(c);
        }
    }
    const std::size_t batch = std::max<std::size_t>(1, opts.swapBatch);
    std::vector<std::size_t> cellStamp(cells.size(), 0);
    std::vector<std::size_t> netStamp(d.nets.size(), 0);
    std::vector<Candidate> found(batch);
    std::size_t applied = 0;
    for (std::size_t first = 0, round = 1; first < cells.size(); first += batch, ++round) {
        const std::size_t count = std::min(batch, cells.size() - first);
        parallelFor(count, 1, [&](std::size_t k) {
            found[k] = getBestSwap(first + k);
        });
        for (std::size_t k = 0; k < count; ++k) {
            const std::size_t i = first + k;
            const Candidate &c = found[k];
            if (c.other == noCell) {
                continue;
            }
            const std::size_t j = c.other;
            const auto touched = [&](std::size_t cell) {
                return cellStamp[cell] == round ||
                       std::ranges::any_of(cellNets[cell], [&](std::size_t n) {
                           return netStamp[n] == round;
                       });
            };
            if (touched(i) || touched(j)) {
                continue;
            }
            if (c.intoSpace) {
                const Move move{.cell = i, .to = c.to};
                if (!canMoveAfter(i, j, c.to.x) || getDelta(std::span(&move, 1)) >= -gain) {
                    continue;
                }
                applyMoveAfter(i, j, c.to.x);
            } else {
                const Move moves[2] = {Move{.cell = i, .to = getPosition(j)},
                                       Move{.cell = j, .to = getPosition(i)}};
                if (!canSwap(i, j) || getDelta(moves) >= -gain) {
                    continue;
                }
                applySwap(i, j);
            }
            ++applied;
            for (const std::size_t cell : {i, j}) {
                cellStamp[cell] = round;
                for (const std::size_t n : cellNets[cell]) {
                    netStamp[n] = round;
                }
            }
        }
    }
    return applied;
}

void AbcdPlacer::Impl::writeFrame(const std::string &note, std::size_t step,
                                  double hpwlInitial) const {
    if (!d.onFrame) {
        return;
    }
    std::vector<float> fx(d.vertexX.size());
    std::vector<float> fy(d.vertexY.size());
    for (std::size_t v = 0; v < fx.size(); ++v) {
        fx[v] = static_cast<float>(d.vertexX[v]);
        fy[v] = static_cast<float>(d.vertexY[v]);
    }
    for (std::size_t c = 0; c < d.cells.size(); ++c) {
        fx[d.cells.id[c]] = static_cast<float>(d.cells.x[c]);
        fy[d.cells.id[c]] = static_cast<float>(d.cells.y[c]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.numSteps = 4;
    info.hpwl = getHpwl();
    info.hpwlInitial = hpwlInitial;
    info.mandatory = true;
    d.onFrame(fx, fy, info);
}

void AbcdPlacer::Impl::place(const AbcdplaceOptions &options) {
    ScopedTimer timer("abcdplace");
    opts = options;
    blocked = d.blockages;
    buildSegments();
    assignCells();
    std::vector<char> stranded(d.cells.size(), 0);
    for (std::size_t c = 0; c < d.cells.size(); ++c) {
        if (segmentOf[c] == noCell) {
            stranded[c] = 1;
            blocked.push_back(
                BBox::of(d.cells.x[c], d.cells.y[c], d.cells.width[c], d.cells.height[c]));
        }
    }
    const auto strandedCells = static_cast<std::size_t>(std::ranges::count(stranded, 1));
    if (strandedCells > 0) {
        buildSegments();
        assignCells();
        for (std::size_t c = 0; c < d.cells.size(); ++c) {
            if (stranded[c] != 0 && segmentOf[c] != noCell) {
                Segment &seg = segments[segmentOf[c]];
                seg.cells.erase(seg.cells.begin() + static_cast<std::ptrdiff_t>(indexInSegment[c]));
                for (std::size_t k = indexInSegment[c]; k < seg.cells.size(); ++k) {
                    indexInSegment[seg.cells[k]] = k;
                }
                segmentOf[c] = noCell;
                indexInSegment[c] = noCell;
            }
        }
        ktlog.warning("ABCDPlace: {} cell(s) are not on a row and stay where they are",
                      strandedCells);
    }
    indexNets();

    const double initial = getHpwl();
    writeFrame("Input - legal placement", 0, initial);
    std::vector<Step> steps;
    const auto run = [&](const std::string &name, auto &&body) {
        const auto start = std::chrono::steady_clock::now();
        const std::size_t moves = body();
        steps.push_back(Step{
            .name = name,
            .hpwl = getHpwl(),
            .seconds =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
            .moves = moves});
        writeFrame("ABCDPlace - " + name, steps.size(), initial);
    };
    run("local reordering", [&] {
        return reorderLocally();
    });
    run("independent set matching", [&] {
        std::size_t moves = 0;
        double before = getHpwl();
        for (std::size_t it = 0; it < opts.ismIterations; ++it) {
            moves += matchIndependentSets(it);
            const double after = getHpwl();
            if (before - after < opts.ismMinImprovement * before) {
                break;
            }
            before = after;
        }
        return moves;
    });
    run("global swap", [&] {
        return swapGlobally();
    });
    run("local reordering", [&] {
        return reorderLocally();
    });
    report(steps, initial);
}

AbcdPlacer::AbcdPlacer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

AbcdPlacer::~AbcdPlacer() = default;

void AbcdPlacer::place(const AbcdplaceOptions &options) {
    pImpl->place(options);
}

}  // namespace ktplace::abcdplace
