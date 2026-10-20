#include "detailPlacer/fastdp/fastdp_placer.h"

#include "detailPlacer/fastdp/fastdp_options.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ktplace::fastdp {

namespace {

constexpr std::size_t noSlot = std::numeric_limits<std::size_t>::max();
constexpr double eps = 1e-6;
constexpr double inf = std::numeric_limits<double>::infinity();

struct Span {
    double ylo = 0.0;
    double yhi = 0.0;
    double xlo = 0.0;
    double xhi = 0.0;
    double site = 1.0;
    std::size_t row = 0;
    int region = noRegion;
    std::vector<std::size_t> cells;

    [[nodiscard]] double width() const noexcept {
        return std::floor(xhi / site + eps) * site - std::ceil(xlo / site - eps) * site;
    }
    [[nodiscard]] bool contains(double px, double pw) const noexcept {
        return px >= xlo - eps && px + pw <= xhi + eps;
    }
};

struct NetPin {
    std::size_t slot = noSlot;
    double offX = 0.0;
    double offY = 0.0;
    double absX = 0.0;
    double absY = 0.0;
};

struct FixedBox {
    double y0 = 0.0;
    double y1 = 0.0;
    double x0 = 0.0;
    double x1 = 0.0;
};

struct Interval {
    double lo = 0.0;
    double hi = 0.0;

    [[nodiscard]] bool contains(double v) const noexcept {
        return v >= lo - eps && v <= hi + eps;
    }
    [[nodiscard]] double center() const noexcept {
        return 0.5 * (lo + hi);
    }
};

struct OptimalRegion {
    Interval x;
    Interval y;
};

struct Move {
    std::size_t cell = noSlot;
    double x = 0.0;
    double y = 0.0;
};

struct BoundingBox {
    double xlo = inf;
    double xhi = -inf;
    double ylo = inf;
    double yhi = -inf;

    void add(double x, double y) noexcept {
        xlo = std::min(xlo, x);
        xhi = std::max(xhi, x);
        ylo = std::min(ylo, y);
        yhi = std::max(yhi, y);
    }
    [[nodiscard]] double halfPerimeter() const noexcept {
        return xlo > xhi ? 0.0 : (xhi - xlo) + (yhi - ylo);
    }
};

Interval medianInterval(std::vector<double> &values) {
    const std::size_t k = values.size() / 2;
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(k));
    const double hi = values[k];
    const double lo =
        k == 0 ? hi
               : *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k));
    return {lo, hi};
}

void keepCentral(std::vector<double> &bounds, std::size_t keep) {
    std::ranges::stable_sort(bounds);
    if (bounds.size() > keep) {
        const std::size_t drop = (bounds.size() - keep) / 2;
        bounds.erase(bounds.end() - static_cast<std::ptrdiff_t>(bounds.size() - keep - drop),
                     bounds.end());
        bounds.erase(bounds.begin(), bounds.begin() + static_cast<std::ptrdiff_t>(drop));
    }
}

double snapWithin(double x, double site, double w, double lo, double hi) {
    const double first = std::ceil(lo / site - eps) * site;
    const double last = std::floor((hi - w) / site + eps) * site;
    return std::clamp(std::round(x / site) * site, first, std::max(first, last));
}

double secondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

class FastDetailedPlacer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    FastdpResult place(const FastdpOptions &params);

    Design &design;
    std::vector<std::size_t> mov;
    std::vector<double> cellX;
    std::vector<double> cellY;

private:
    struct Window {
        std::size_t span = noSlot;
        std::size_t first = 0;
        std::size_t last = 0;
        double lo = 0.0;
        double hi = 0.0;
    };

    struct Candidate {
        double benefit = 0.0;
        std::size_t partner = noSlot;
        std::size_t span = noSlot;
        double x = 0.0;
        double partnerX = 0.0;
    };

    struct DeltaScratch {
        std::vector<std::size_t> moveOf;
        std::vector<std::uint32_t> netStamp;
        std::uint32_t stamp = 0;
    };

    struct Plan {
        bool active = false;
        double targetX = 0.0;
        std::vector<Candidate> found;
    };

    void collectCells(double rowHeight);
    void buildSpans();
    void assignCells();
    void indexFixed();
    void buildNetlist();

    [[nodiscard]] double hpwl() const;
    [[nodiscard]] double wirelengthDelta(std::span<const Move> moves) const;
    [[nodiscard]] double wirelengthDelta(std::span<const Move> moves, DeltaScratch &sc) const;
    [[nodiscard]] DeltaScratch freshScratch() const;
    [[nodiscard]] OptimalRegion optimalRegion(std::size_t c) const;

    [[nodiscard]] std::size_t indexIn(std::size_t s, std::size_t c) const;
    [[nodiscard]] double gap(std::size_t s, std::ptrdiff_t g) const;
    [[nodiscard]] double snap(double x, std::size_t s, double w) const;
    [[nodiscard]] double penalty(double excess, double nearRoom, double wideRoom) const;
    [[nodiscard]] double cellPenalty(std::size_t s, std::size_t k, double excess) const;
    [[nodiscard]] std::size_t nearestRow(double y) const;

    [[nodiscard]] Candidate swapWith(std::size_t i, std::size_t j, DeltaScratch &sc) const;
    [[nodiscard]] Candidate moveInto(std::size_t i, std::size_t s, std::size_t g, double target,
                                     DeltaScratch &sc) const;
    [[nodiscard]] std::size_t gapAt(std::size_t s, double x) const;
    [[nodiscard]] Plan globalPlan(std::size_t i, DeltaScratch &sc) const;
    [[nodiscard]] Plan verticalPlan(std::size_t i, DeltaScratch &sc) const;
    [[nodiscard]] bool commit(std::size_t i, const Plan &plan);
    std::size_t sweepCells(Plan (Impl::*plan)(std::size_t, DeltaScratch &) const);
    void apply(std::size_t i, const Candidate &c);
    [[nodiscard]] bool overlapsOthers(std::size_t s, double x, double w, const Candidate &c,
                                      std::size_t i) const;
    [[nodiscard]] bool overlapsAfter(std::size_t i, const Candidate &c) const;
    [[nodiscard]] bool applyIfShorter(std::size_t i, const Candidate &c);
    [[nodiscard]] bool applyFirstShorter(std::size_t i, std::vector<Candidate> &found);
    void relocate(std::size_t c, double nx, std::size_t s);
    void detach(std::size_t c);
    void attach(std::size_t c, std::size_t s);
    [[nodiscard]] Window windowAt(std::size_t s, std::size_t k) const;
    [[nodiscard]] Window wholeSpan(std::size_t s) const;
    void legalizeWindow(const Window &w);

    std::size_t globalSwap();
    std::size_t verticalSwap();
    std::size_t keepShorter(std::size_t (Impl::*sweep)());
    void legalizeBatch();
    std::size_t localReorder();
    std::size_t clusterSegments();
    std::size_t clusterWindow(const Window &w);

    void selfCheck(FastdpResult &res) const;

    struct Snapshot {
        std::vector<double> x;
        std::vector<double> y;
        std::vector<std::size_t> spanOf;
        std::vector<Span> spans;
        std::vector<double> used;
    };
    [[nodiscard]] Snapshot snapshot() const;
    void restore(Snapshot saved);
    void writeFrame(const std::string &note, std::size_t step = 0) const;
    double hpwlInput = 0.0;

    FastdpOptions opts;
    std::vector<double> cellW;
    std::vector<double> cellH;
    std::vector<int> cellRegion;
    std::vector<char> movable;
    std::vector<char> stranded;
    std::vector<std::size_t> spanOf;
    std::vector<Span> spans;
    std::vector<double> used;
    std::vector<std::vector<std::size_t>> spansInRow;
    std::vector<double> rowY;
    std::vector<std::vector<NetPin>> netPins;
    std::vector<std::vector<std::size_t>> cellNets;
    std::vector<FixedBox> fixedBoxes;
    std::vector<std::vector<std::size_t>> fixedByBand;
    double bandY = inf;
    double bandY0 = 0.0;
    std::size_t tallSkipped = 0;

    std::vector<std::size_t> orderOf;
    bool batched = false;
    std::vector<std::size_t> batchSpans;
    mutable DeltaScratch deltaScratch;
};

void FastDetailedPlacer::Impl::collectCells(double rowHeight) {
    movable.assign(design.cells.size(), 0);
    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        const Cell &cell = design.cells[v];
        const bool tall = cell.height > rowHeight + eps;
        if (cell.isFixed || tall) {
            fixedBoxes.push_back(
                FixedBox{cell.y, cell.y + cell.height, cell.x, cell.x + cell.width});
        }
        if (cell.isFixed || cell.isTerminal || tall) {
            tallSkipped += (tall && !cell.isFixed && !cell.isTerminal) ? 1 : 0;
            continue;
        }
        movable[v] = 1;
        mov.push_back(v);
        cellW.push_back(cell.width);
        cellH.push_back(cell.height);
        cellX.push_back(cell.x);
        cellY.push_back(cell.y);
        cellRegion.push_back(cell.region);
    }
}

void FastDetailedPlacer::Impl::buildSpans() {
    std::vector<RowInfo> rows(design.rows);
    std::ranges::stable_sort(rows, {}, &RowInfo::coordinate);
    rowY.clear();
    for (const RowInfo &ri : rows) {
        rowY.push_back(ri.coordinate);
    }
    stranded.assign(mov.size(), 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<std::vector<std::pair<double, double>>> blocked(rows.size());
        const auto block = [&](double x0, double x1, double y0, double y1) {
            auto r = std::ranges::upper_bound(rows, y0 + eps, {}, [](const RowInfo &ri) {
                return ri.coordinate + ri.height;
            });
            for (; r != rows.end() && r->coordinate < y1 - eps; ++r) {
                blocked[static_cast<std::size_t>(r - rows.begin())].emplace_back(x0, x1);
            }
        };
        for (std::size_t v = 0; v < design.cells.size(); ++v) {
            const Cell &cell = design.cells[v];
            if (!movable[v] && cell.width > 0.0 && cell.height > 0.0) {
                block(cell.x, cell.x + cell.width, cell.y, cell.y + cell.height);
            }
        }
        for (std::size_t i = 0; i < mov.size(); ++i) {
            if (stranded[i]) {
                block(cellX[i], cellX[i] + cellW[i], cellY[i], cellY[i] + cellH[i]);
            }
        }

        spans.clear();
        spansInRow.assign(rows.size(), {});
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const RowInfo &ri = rows[r];
            std::vector<std::pair<double, double>> &obs = blocked[r];
            std::ranges::stable_sort(obs);
            for (const Subrow &sub : ri.subrows) {
                const auto emit = [&](double lo, double hi) {
                    if (hi > lo + eps) {
                        spansInRow[r].push_back(spans.size());
                        spans.push_back(Span{ri.coordinate,
                                             ri.coordinate + ri.height,
                                             lo,
                                             hi,
                                             ri.pitch() > 0.0 ? ri.pitch() : 1.0,
                                             r,
                                             sub.region,
                                             {}});
                    }
                };
                double cursor = sub.xlo;
                for (const auto &[o0, o1] : obs) {
                    if (o1 <= cursor + eps || o0 >= sub.xhi - eps) {
                        continue;
                    }
                    emit(cursor, std::min(o0, sub.xhi));
                    cursor = std::max(cursor, o1);
                }
                emit(cursor, sub.xhi);
            }
        }
        assignCells();
        if (attempt == 1 || std::ranges::none_of(spanOf, [](std::size_t s) {
                return s == noSlot;
            })) {
            break;
        }
        for (std::size_t i = 0; i < mov.size(); ++i) {
            stranded[i] = spanOf[i] == noSlot ? 1 : 0;
        }
    }
    used.assign(spans.size(), 0.0);
    for (std::size_t s = 0; s < spans.size(); ++s) {
        for (const std::size_t c : spans[s].cells) {
            used[s] += cellW[c];
        }
    }
}

void FastDetailedPlacer::Impl::assignCells() {
    spanOf.assign(mov.size(), noSlot);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        if (stranded[i]) {
            continue;
        }
        const std::size_t r = nearestRow(cellY[i]);
        if (r == noSlot || std::abs(rowY[r] - cellY[i]) > eps) {
            continue;
        }
        for (const std::size_t s : spansInRow[r]) {
            if (spans[s].region == cellRegion[i] && spans[s].contains(cellX[i], cellW[i])) {
                spanOf[i] = s;
                spans[s].cells.push_back(i);
                break;
            }
        }
    }
    for (Span &sp : spans) {
        std::ranges::stable_sort(sp.cells, {}, [this](std::size_t c) {
            return cellX[c];
        });
    }
}

void FastDetailedPlacer::Impl::indexFixed() {
    bandY0 = rowY.empty() ? 0.0 : rowY.front();
    for (const RowInfo &ri : design.rows) {
        if (ri.height > 0.0) {
            bandY = std::min(bandY, ri.height);
        }
    }
    if (!std::isfinite(bandY)) {
        bandY = 1.0;
    }
    const std::size_t bands = design.rows.size() + 2;
    fixedByBand.assign(bands, {});
    const auto band = [&](double y) {
        return std::clamp<long>(static_cast<long>(std::floor((y - bandY0) / bandY)), 0,
                                static_cast<long>(bands) - 1);
    };
    for (std::size_t i = 0; i < fixedBoxes.size(); ++i) {
        for (long b = band(fixedBoxes[i].y0); b <= band(fixedBoxes[i].y1); ++b) {
            fixedByBand[static_cast<std::size_t>(b)].push_back(i);
        }
    }
}

void FastDetailedPlacer::Impl::buildNetlist() {
    std::vector<std::size_t> slotOf(design.cells.size(), noSlot);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        slotOf[mov[i]] = i;
    }
    netPins.assign(design.nets.size(), {});
    cellNets.assign(mov.size(), {});
    for (std::size_t n = 0; n < design.nets.size(); ++n) {
        for (const std::size_t pinId : design.nets[n].pins) {
            const Pin &pin = design.pins[pinId];
            const std::size_t s = slotOf[pin.cellId];
            const Cell &c = design.cells[pin.cellId];
            if (s == noSlot) {
                netPins[n].push_back(
                    NetPin{noSlot, pin.offsetX, pin.offsetY, c.x + pin.offsetX, c.y + pin.offsetY});
                continue;
            }
            netPins[n].push_back(NetPin{s, pin.offsetX, pin.offsetY, 0.0, 0.0});
            if (cellNets[s].empty() || cellNets[s].back() != n) {
                cellNets[s].push_back(n);
            }
        }
    }
    deltaScratch = freshScratch();
    orderOf.assign(mov.size(), noSlot);
}

double FastDetailedPlacer::Impl::hpwl() const {
    double total = 0.0;
    for (const std::vector<NetPin> &pins : netPins) {
        BoundingBox box;
        for (const NetPin &p : pins) {
            if (p.slot == noSlot) {
                box.add(p.absX, p.absY);
            } else {
                box.add(cellX[p.slot] + p.offX, cellY[p.slot] + p.offY);
            }
        }
        total += box.halfPerimeter();
    }
    return total;
}

FastDetailedPlacer::Impl::DeltaScratch FastDetailedPlacer::Impl::freshScratch() const {
    return {.moveOf = std::vector<std::size_t>(mov.size(), noSlot),
            .netStamp = std::vector<std::uint32_t>(design.nets.size(), 0),
            .stamp = 0};
}

double FastDetailedPlacer::Impl::wirelengthDelta(std::span<const Move> moves) const {
    return wirelengthDelta(moves, deltaScratch);
}

double FastDetailedPlacer::Impl::wirelengthDelta(std::span<const Move> moves,
                                                 DeltaScratch &sc) const {
    std::vector<std::size_t> &moveOf = sc.moveOf;
    std::vector<std::uint32_t> &netStamp = sc.netStamp;
    if (++sc.stamp == 0) {
        std::ranges::fill(netStamp, 0u);
        sc.stamp = 1;
    }
    const std::uint32_t stamp = sc.stamp;
    for (std::size_t k = 0; k < moves.size(); ++k) {
        moveOf[moves[k].cell] = k;
    }
    double delta = 0.0;
    for (const Move &m : moves) {
        for (const std::size_t n : cellNets[m.cell]) {
            if (netStamp[n] == stamp) {
                continue;
            }
            netStamp[n] = stamp;
            BoundingBox before;
            BoundingBox after;
            for (const NetPin &p : netPins[n]) {
                if (p.slot == noSlot) {
                    before.add(p.absX, p.absY);
                    after.add(p.absX, p.absY);
                    continue;
                }
                before.add(cellX[p.slot] + p.offX, cellY[p.slot] + p.offY);
                if (const std::size_t k = moveOf[p.slot]; k != noSlot) {
                    after.add(moves[k].x + p.offX, moves[k].y + p.offY);
                } else {
                    after.add(cellX[p.slot] + p.offX, cellY[p.slot] + p.offY);
                }
            }
            delta += after.halfPerimeter() - before.halfPerimeter();
        }
    }
    for (const Move &m : moves) {
        moveOf[m.cell] = noSlot;
    }
    return delta;
}

OptimalRegion FastDetailedPlacer::Impl::optimalRegion(std::size_t c) const {
    std::vector<double> xs;
    std::vector<double> ys;
    for (const std::size_t n : cellNets[c]) {
        BoundingBox others;
        const NetPin *own = nullptr;
        for (const NetPin &p : netPins[n]) {
            if (p.slot == c) {
                own = own != nullptr ? own : &p;
            } else if (p.slot == noSlot) {
                others.add(p.absX, p.absY);
            } else {
                others.add(cellX[p.slot] + p.offX, cellY[p.slot] + p.offY);
            }
        }
        if (own == nullptr || others.xlo > others.xhi) {
            continue;
        }
        xs.insert(xs.end(), {others.xlo - own->offX, others.xhi - own->offX});
        ys.insert(ys.end(), {others.ylo - own->offY, others.yhi - own->offY});
    }
    if (xs.empty()) {
        return {{cellX[c], cellX[c]}, {cellY[c], cellY[c]}};
    }
    return {medianInterval(xs), medianInterval(ys)};
}

std::size_t FastDetailedPlacer::Impl::indexIn(std::size_t s, std::size_t c) const {
    const std::vector<std::size_t> &cells = spans[s].cells;
    auto it = std::ranges::lower_bound(cells, cellX[c] - eps, {}, [this](std::size_t a) {
        return cellX[a];
    });
    while (it != cells.end() && *it != c) {
        ++it;
    }
    if (it == cells.end()) {
        it = std::ranges::find(cells, c);
    }
    return static_cast<std::size_t>(it - cells.begin());
}

double FastDetailedPlacer::Impl::gap(std::size_t s, std::ptrdiff_t g) const {
    const Span &sp = spans[s];
    const auto n = static_cast<std::ptrdiff_t>(sp.cells.size());
    if (g < 0 || g > n) {
        return 0.0;
    }
    double left = sp.xlo;
    if (g > 0) {
        const std::size_t prev = sp.cells[static_cast<std::size_t>(g - 1)];
        left = cellX[prev] + cellW[prev];
    }
    const double right = g == n ? sp.xhi : cellX[sp.cells[static_cast<std::size_t>(g)]];
    return std::max(0.0, right - left);
}

double FastDetailedPlacer::Impl::snap(double x, std::size_t s, double w) const {
    return snapWithin(x, spans[s].site, w, spans[s].xlo, spans[s].xhi);
}

double FastDetailedPlacer::Impl::penalty(double excess, double nearRoom, double wideRoom) const {
    return std::max(0.0, excess - nearRoom) * opts.penaltyWeight1 +
           std::max(0.0, excess - wideRoom) * opts.penaltyWeight2;
}

double FastDetailedPlacer::Impl::cellPenalty(std::size_t s, std::size_t k, double excess) const {
    if (excess <= 0.0) {
        return 0.0;
    }
    const auto g = static_cast<std::ptrdiff_t>(k);
    const double nearRoom = gap(s, g) + gap(s, g + 1);
    return penalty(excess, nearRoom, nearRoom + gap(s, g - 1) + gap(s, g + 2));
}

std::size_t FastDetailedPlacer::Impl::nearestRow(double y) const {
    if (rowY.empty()) {
        return noSlot;
    }
    const auto it = std::ranges::lower_bound(rowY, y);
    if (it == rowY.end()) {
        return rowY.size() - 1;
    }
    if (it != rowY.begin() && y - *std::prev(it) < *it - y) {
        return static_cast<std::size_t>(it - rowY.begin()) - 1;
    }
    return static_cast<std::size_t>(it - rowY.begin());
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::swapWith(std::size_t i, std::size_t j,
                                                                       DeltaScratch &sc) const {
    const std::size_t si = spanOf[i];
    const std::size_t sj = spanOf[j];
    if (j == i || sj == noSlot || spans[si].region != cellRegion[j]) {
        return {};
    }
    if (si != sj && (used[sj] - cellW[j] + cellW[i] > spans[sj].width() + eps ||
                     used[si] - cellW[i] + cellW[j] > spans[si].width() + eps)) {
        return {};
    }
    const double xi = snap(cellX[j] + 0.5 * (cellW[j] - cellW[i]), sj, cellW[i]);
    const double xj = snap(cellX[i] + 0.5 * (cellW[i] - cellW[j]), si, cellW[j]);
    const std::array<Move, 2> moves{Move{i, xi, spans[sj].ylo}, Move{j, xj, spans[si].ylo}};
    const double overlap = cellW[i] > cellW[j]
                               ? cellPenalty(sj, indexIn(sj, j), cellW[i] - cellW[j])
                               : cellPenalty(si, indexIn(si, i), cellW[j] - cellW[i]);
    return {-wirelengthDelta(moves, sc) - overlap, j, sj, xi, xj};
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::moveInto(std::size_t i, std::size_t s,
                                                                       std::size_t g, double target,
                                                                       DeltaScratch &sc) const {
    if (s != spanOf[i] && used[s] + cellW[i] > spans[s].width() + eps) {
        return {};
    }
    const Span &sp = spans[s];
    const auto gi = static_cast<std::ptrdiff_t>(g);
    const double room = gap(s, gi);
    const double left = g == 0 ? sp.xlo : cellX[sp.cells[g - 1]] + cellW[sp.cells[g - 1]];
    const double x = room >= cellW[i]
                         ? snap(std::clamp(target, left, left + room - cellW[i]), s, cellW[i])
                         : snap(left + 0.5 * (room - cellW[i]), s, cellW[i]);
    const std::array<Move, 1> moves{Move{i, x, sp.ylo}};
    const double overlap =
        room >= cellW[i] ? 0.0 : penalty(cellW[i], room, room + gap(s, gi - 1) + gap(s, gi + 1));
    return {-wirelengthDelta(moves, sc) - overlap, noSlot, s, x, 0.0};
}

FastDetailedPlacer::Impl::Plan FastDetailedPlacer::Impl::globalPlan(std::size_t i,
                                                                    DeltaScratch &sc) const {
    if (spanOf[i] == noSlot) {
        return {};
    }
    const OptimalRegion region = optimalRegion(i);
    if (region.x.contains(cellX[i]) && region.y.contains(cellY[i])) {
        return {};
    }
    const double cx = region.x.center();
    const double cy = region.y.center();
    std::vector<std::size_t> rows;
    const auto lo = std::ranges::lower_bound(rowY, region.y.lo - eps);
    const auto hi = std::ranges::upper_bound(rowY, region.y.hi + eps);
    for (auto r = lo; r != hi; ++r) {
        rows.push_back(static_cast<std::size_t>(r - rowY.begin()));
    }
    if (rows.empty()) {
        rows.push_back(nearestRow(cy));
    }
    std::ranges::stable_sort(rows, {}, [&](std::size_t r) {
        return std::abs(rowY[r] - cy);
    });

    struct Item {
        double distance;
        std::size_t index;
        bool isGap;
    };
    std::vector<Item> items;
    std::size_t budget = opts.swapCandidates;
    std::vector<Candidate> found;
    for (const std::size_t r : rows) {
        for (const std::size_t s : spansInRow[r]) {
            const Span &sp = spans[s];
            if (sp.region != cellRegion[i] || sp.xhi < region.x.lo ||
                sp.xlo > region.x.hi + cellW[i]) {
                continue;
            }
            const std::vector<std::size_t> &cells = sp.cells;
            const auto at =
                static_cast<std::size_t>(std::ranges::lower_bound(cells, cx, {},
                                                                  [this](std::size_t c) {
                                                                      return cellX[c];
                                                                  }) -
                                         cells.begin());
            const std::size_t first = at > budget ? at - budget : 0;
            const std::size_t last = std::min(cells.size(), at + budget);
            items.clear();
            for (std::size_t k = first; k < last; ++k) {
                if (region.x.contains(cellX[cells[k]])) {
                    items.push_back({std::abs(cellX[cells[k]] - cx), k, false});
                }
            }
            for (std::size_t g = first; g <= last; ++g) {
                const double left = g == 0 ? sp.xlo : cellX[cells[g - 1]] + cellW[cells[g - 1]];
                const double right = g == cells.size() ? sp.xhi : cellX[cells[g]];
                if (right > left + eps && right >= region.x.lo - eps && left <= region.x.hi + eps) {
                    items.push_back({std::abs(std::clamp(cx, left, right) - cx), g, true});
                }
            }
            std::ranges::stable_sort(items, {}, &Item::distance);
            for (const Item &item : items) {
                const Candidate c = item.isGap ? moveInto(i, s, item.index, cx, sc)
                                               : swapWith(i, cells[item.index], sc);
                if (c.span != noSlot && c.benefit > eps) {
                    found.push_back(c);
                }
                if (--budget == 0) {
                    break;
                }
            }
            if (budget == 0) {
                break;
            }
        }
        if (budget == 0) {
            break;
        }
    }
    std::ranges::stable_sort(found, std::ranges::greater{}, &Candidate::benefit);
    return {.active = true, .targetX = cx, .found = std::move(found)};
}

std::size_t FastDetailedPlacer::Impl::gapAt(std::size_t s, double x) const {
    const std::vector<std::size_t> &cells = spans[s].cells;
    return static_cast<std::size_t>(std::ranges::lower_bound(cells, x - eps, {},
                                                             [this](std::size_t c) {
                                                                 return cellX[c];
                                                             }) -
                                    cells.begin());
}

bool FastDetailedPlacer::Impl::commit(std::size_t i, const Plan &plan) {
    if (!plan.active || spanOf[i] == noSlot) {
        return false;
    }
    std::vector<Candidate> fresh;
    const std::size_t limit = std::min(plan.found.size(), opts.refreshCandidates);
    for (std::size_t k = 0; k < limit; ++k) {
        const Candidate &c = plan.found[k];
        const Candidate now = c.partner != noSlot ? swapWith(i, c.partner, deltaScratch)
                                                  : moveInto(i, c.span, gapAt(c.span, c.x),
                                                             plan.targetX, deltaScratch);
        if (now.span != noSlot && now.benefit > eps) {
            fresh.push_back(now);
        }
    }
    return applyFirstShorter(i, fresh);
}

std::size_t FastDetailedPlacer::Impl::sweepCells(Plan (Impl::*plan)(std::size_t, DeltaScratch &)
                                                     const) {
    tbb::enumerable_thread_specific<DeltaScratch> local([this] {
        return freshScratch();
    });
    const std::size_t batch = std::max<std::size_t>(opts.parallelBatch, 1);
    std::vector<Plan> plans;
    std::size_t moves = 0;
    for (std::size_t begin = 0; begin < mov.size(); begin += batch) {
        const std::size_t end = std::min(mov.size(), begin + batch);
        plans.assign(end - begin, Plan{});
        tbb::parallel_for(tbb::blocked_range<std::size_t>(begin, end),
                          [&](const tbb::blocked_range<std::size_t> &range) {
                              DeltaScratch &sc = local.local();
                              for (std::size_t i = range.begin(); i < range.end(); ++i) {
                                  plans[i - begin] = (this->*plan)(i, sc);
                              }
                          });
        for (std::size_t i = begin; i < end; ++i) {
            moves += commit(i, plans[i - begin]) ? 1 : 0;
        }
    }
    return moves;
}

void FastDetailedPlacer::Impl::apply(std::size_t i, const Candidate &c) {
    const std::size_t si = spanOf[i];
    if (c.partner != noSlot) {
        relocate(c.partner, c.partnerX, si);
    }
    relocate(i, c.x, c.span);
}

bool FastDetailedPlacer::Impl::applyFirstShorter(std::size_t i, std::vector<Candidate> &found) {
    std::ranges::stable_sort(found, std::ranges::greater{}, &Candidate::benefit);
    if (batched) {
        if (found.empty()) {
            return false;
        }
        batchSpans.insert(batchSpans.end(), {spanOf[i], found.front().span});
        apply(i, found.front());
        return true;
    }
    const std::size_t tries = std::min(found.size(), opts.verifiedCandidates);
    for (std::size_t k = 0; k < tries; ++k) {
        if (applyIfShorter(i, found[k])) {
            return true;
        }
    }
    return false;
}

bool FastDetailedPlacer::Impl::overlapsOthers(std::size_t s, double x, double w, const Candidate &c,
                                              std::size_t i) const {
    const std::vector<std::size_t> &cells = spans[s].cells;
    const auto at = std::ranges::lower_bound(cells, x, {}, [this](std::size_t a) {
        return cellX[a];
    });
    const auto moving = [&](std::size_t a) {
        return a == i || a == c.partner;
    };
    for (auto r = at; r != cells.end(); ++r) {
        if (!moving(*r)) {
            if (x + w > cellX[*r] + eps) {
                return true;
            }
            break;
        }
    }
    for (auto l = at; l != cells.begin();) {
        --l;
        if (!moving(*l)) {
            return cellX[*l] + cellW[*l] > x + eps;
        }
    }
    return false;
}

bool FastDetailedPlacer::Impl::overlapsAfter(std::size_t i, const Candidate &c) const {
    if (overlapsOthers(c.span, c.x, cellW[i], c, i)) {
        return true;
    }
    if (c.partner == noSlot) {
        return false;
    }
    const std::size_t home = spanOf[i];
    const double pw = cellW[c.partner];
    return overlapsOthers(home, c.partnerX, pw, c, i) ||
           (home == c.span && c.x < c.partnerX + pw - eps && c.partnerX < c.x + cellW[i] - eps);
}

bool FastDetailedPlacer::Impl::applyIfShorter(std::size_t i, const Candidate &c) {
    if (!overlapsAfter(i, c)) {
        apply(i, c);
        return true;
    }
    const std::size_t home = spanOf[i];
    const std::size_t vacated = indexIn(home, i);
    const std::array<double, 2> usedBefore{used[home], used[c.span]};
    std::vector<Move> before{Move{i, cellX[i], cellY[i]}};
    if (c.partner != noSlot) {
        before.push_back(Move{c.partner, cellX[c.partner], cellY[c.partner]});
    }

    apply(i, c);
    std::vector<std::pair<std::size_t, std::size_t>> anchors{{c.span, indexIn(c.span, i)}};
    if (c.partner != noSlot) {
        anchors.emplace_back(home, indexIn(home, c.partner));
    } else if (!spans[home].cells.empty()) {
        anchors.emplace_back(home, std::min(vacated, spans[home].cells.size() - 1));
    }
    for (const auto &[span, index] : anchors) {
        const Window w = windowAt(span, index);
        const std::vector<std::size_t> &cells = spans[w.span].cells;
        for (std::size_t k = w.first; k < w.last; ++k) {
            if (std::ranges::find(before, cells[k], &Move::cell) == before.end()) {
                before.push_back(Move{cells[k], cellX[cells[k]], cellY[cells[k]]});
            }
        }
        legalizeWindow(w);
        clusterWindow(w);
    }
    std::erase_if(before, [this](const Move &m) {
        return std::abs(cellX[m.cell] - m.x) <= eps && std::abs(cellY[m.cell] - m.y) <= eps;
    });
    if (wirelengthDelta(before) > eps) {
        return true;
    }

    detach(i);
    if (c.partner != noSlot) {
        detach(c.partner);
    }
    for (const Move &m : before) {
        cellX[m.cell] = m.x;
        cellY[m.cell] = m.y;
    }
    attach(i, home);
    if (c.partner != noSlot) {
        attach(c.partner, c.span);
    }
    used[home] = usedBefore[0];
    used[c.span] = usedBefore[1];
    return false;
}

void FastDetailedPlacer::Impl::relocate(std::size_t c, double nx, std::size_t s) {
    if (spanOf[c] != noSlot) {
        detach(c);
    }
    cellX[c] = nx;
    attach(c, s);
}

void FastDetailedPlacer::Impl::detach(std::size_t c) {
    const std::size_t s = spanOf[c];
    std::vector<std::size_t> &cells = spans[s].cells;
    cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(indexIn(s, c)));
    used[s] -= cellW[c];
    spanOf[c] = noSlot;
}

void FastDetailedPlacer::Impl::attach(std::size_t c, std::size_t s) {
    cellY[c] = spans[s].ylo;
    std::vector<std::size_t> &cells = spans[s].cells;
    cells.insert(std::ranges::upper_bound(cells, cellX[c], {},
                                          [this](std::size_t a) {
                                              return cellX[a];
                                          }),
                 c);
    used[s] += cellW[c];
    spanOf[c] = s;
}

FastDetailedPlacer::Impl::Window FastDetailedPlacer::Impl::wholeSpan(std::size_t s) const {
    return {s, 0, spans[s].cells.size(), spans[s].xlo, spans[s].xhi};
}

FastDetailedPlacer::Impl::Window FastDetailedPlacer::Impl::windowAt(std::size_t s,
                                                                    std::size_t k) const {
    const Span &sp = spans[s];
    const std::vector<std::size_t> &cells = sp.cells;
    for (std::size_t reach = std::max<std::size_t>(opts.verifyReach, 1);; reach *= 2) {
        const std::size_t first = k > reach ? k - reach : 0;
        const std::size_t last = std::min(cells.size(), k + reach + 1);
        if (first == 0 && last == cells.size()) {
            return wholeSpan(s);
        }
        const double lo = first == 0 ? sp.xlo : cellX[cells[first - 1]] + cellW[cells[first - 1]];
        const double hi = last == cells.size() ? sp.xhi : cellX[cells[last]];
        double widths = 0.0;
        for (std::size_t m = first; m < last; ++m) {
            widths += cellW[cells[m]];
        }
        const double room =
            std::floor(hi / sp.site + eps) * sp.site - std::ceil(lo / sp.site - eps) * sp.site;
        if (widths <= room + eps) {
            return {s, first, last, lo, hi};
        }
    }
}

void FastDetailedPlacer::Impl::legalizeWindow(const Window &w) {
    if (w.first == w.last) {
        return;
    }
    std::vector<std::size_t> &cells = spans[w.span].cells;
    const double site = spans[w.span].site;
    std::stable_sort(cells.begin() + static_cast<std::ptrdiff_t>(w.first),
                     cells.begin() + static_cast<std::ptrdiff_t>(w.last),
                     [this](std::size_t a, std::size_t b) {
                         return cellX[a] < cellX[b];
                     });
    struct Cluster {
        double weight;
        double q;
        double width;
        double x;
        std::size_t first;
    };
    std::vector<Cluster> clusters;
    const auto place = [&](Cluster &cl) {
        cl.x = snapWithin(cl.q / cl.weight, site, cl.width, w.lo, w.hi);
    };
    for (std::size_t k = w.first; k < w.last; ++k) {
        const std::size_t c = cells[k];
        Cluster cur{1.0, cellX[c], cellW[c], 0.0, k};
        place(cur);
        while (!clusters.empty() && clusters.back().x + clusters.back().width > cur.x + eps) {
            Cluster prev = clusters.back();
            clusters.pop_back();
            prev.q += cur.q - cur.weight * prev.width;
            prev.weight += cur.weight;
            prev.width += cur.width;
            place(prev);
            cur = prev;
        }
        clusters.push_back(cur);
    }
    for (std::size_t m = 0; m < clusters.size(); ++m) {
        const std::size_t end = m + 1 < clusters.size() ? clusters[m + 1].first : w.last;
        double x = clusters[m].x;
        for (std::size_t k = clusters[m].first; k < end; ++k) {
            cellX[cells[k]] = x;
            x += cellW[cells[k]];
        }
    }
}

std::size_t FastDetailedPlacer::Impl::keepShorter(std::size_t (Impl::*sweep)()) {
    const double before = hpwl();
    Snapshot saved = snapshot();
    batched = true;
    const std::size_t moves = (this->*sweep)();
    batched = false;
    legalizeBatch();
    if (hpwl() < before) {
        return moves;
    }
    restore(std::move(saved));
    return (this->*sweep)();
}

void FastDetailedPlacer::Impl::legalizeBatch() {
    std::ranges::stable_sort(batchSpans);
    const auto [from, to] = std::ranges::unique(batchSpans);
    batchSpans.erase(from, to);
    for (const std::size_t s : batchSpans) {
        if (used[s] <= spans[s].width() + eps) {
            legalizeWindow(wholeSpan(s));
        }
    }
    batchSpans.clear();
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    return sweepCells(&Impl::globalPlan);
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    return sweepCells(&Impl::verticalPlan);
}

FastDetailedPlacer::Impl::Plan FastDetailedPlacer::Impl::verticalPlan(std::size_t i,
                                                                      DeltaScratch &sc) const {
    const std::size_t si = spanOf[i];
    if (si == noSlot) {
        return {};
    }
    const OptimalRegion region = optimalRegion(i);
    const std::size_t row = spans[si].row;
    std::size_t target = noSlot;
    if (region.y.lo > cellY[i] + eps && row + 1 < rowY.size()) {
        target = row + 1;
    } else if (region.y.hi < cellY[i] - eps && row > 0) {
        target = row - 1;
    }
    if (target == noSlot) {
        return {};
    }
    std::vector<Candidate> found;
    for (const std::size_t s : spansInRow[target]) {
        const Span &sp = spans[s];
        if (sp.region != cellRegion[i] || sp.cells.empty() || sp.xhi < cellX[i] ||
            sp.xlo > cellX[i] + cellW[i]) {
            continue;
        }
        const auto at = static_cast<std::size_t>(std::ranges::lower_bound(sp.cells, cellX[i], {},
                                                                          [this](std::size_t c) {
                                                                              return cellX[c];
                                                                          }) -
                                                 sp.cells.begin());
        const std::size_t reach = opts.verticalCandidates;
        const std::size_t first = at > reach ? at - reach : 0;
        const std::size_t last = std::min(sp.cells.size(), at + reach);
        for (std::size_t k = first; k < last; ++k) {
            const Candidate c = swapWith(i, sp.cells[k], sc);
            if (c.span != noSlot && c.benefit > eps) {
                found.push_back(c);
            }
        }
    }
    std::ranges::stable_sort(found, std::ranges::greater{}, &Candidate::benefit);
    return {.active = true, .targetX = 0.0, .found = std::move(found)};
}

std::size_t FastDetailedPlacer::Impl::localReorder() {
    const std::size_t n = std::min<std::size_t>(opts.reorderWindow, 6);
    if (n < 2) {
        return 0;
    }
    std::size_t moves = 0;
    std::vector<std::size_t> order(n);
    std::vector<Move> trial(n);
    std::vector<Move> best(n);
    for (Span &sp : spans) {
        std::vector<std::size_t> &cells = sp.cells;
        for (std::size_t k = 0; k + n <= cells.size(); ++k) {
            const std::span<std::size_t> group(cells.data() + k, n);
            const double left = cellX[group.front()];
            const double right = cellX[group.back()] + cellW[group.back()];
            double widths = 0.0;
            for (const std::size_t c : group) {
                widths += cellW[c];
            }
            const double spacing = (right - left - widths) / static_cast<double>(n - 1);
            if (spacing < -eps) {
                continue;
            }
            std::iota(order.begin(), order.end(), 0u);
            double bestDelta = -eps;
            bool improved = false;
            do {
                double x = left;
                for (std::size_t t = 0; t < n; ++t) {
                    const std::size_t c = group[order[t]];
                    trial[t] =
                        Move{c, left + std::floor((x - left) / sp.site + eps) * sp.site, cellY[c]};
                    x += cellW[c] + spacing;
                }
                if (const double delta = wirelengthDelta(trial); delta < bestDelta) {
                    bestDelta = delta;
                    best = trial;
                    improved = true;
                }
            } while (std::ranges::next_permutation(order).found);
            if (improved) {
                for (std::size_t t = 0; t < n; ++t) {
                    moves += std::abs(cellX[best[t].cell] - best[t].x) > eps ? 1 : 0;
                    cellX[best[t].cell] = best[t].x;
                    group[t] = best[t].cell;
                }
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::clusterSegments() {
    std::size_t moves = 0;
    for (std::size_t s = 0; s < spans.size(); ++s) {
        moves += clusterWindow(wholeSpan(s));
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::clusterWindow(const Window &w) {
    struct Cluster {
        std::vector<double> bounds;
        double width;
        double x;
        std::size_t first;
    };
    std::size_t moves = 0;
    std::vector<Move> placed;
    std::vector<Cluster> clusters;
    const std::vector<std::size_t> &cells = spans[w.span].cells;
    const double site = spans[w.span].site;
    if (w.first == w.last) {
        return 0;
    }
    for (std::size_t k = w.first; k < w.last; ++k) {
        orderOf[cells[k]] = k;
    }
    const auto boundsOf = [&](std::size_t c) {
        std::vector<double> bounds;
        for (const std::size_t n : cellNets[c]) {
            double lo = inf;
            double hi = -inf;
            const NetPin *own = nullptr;
            for (const NetPin &p : netPins[n]) {
                if (p.slot == c) {
                    own = own != nullptr ? own : &p;
                    continue;
                }
                double px = p.absX;
                if (p.slot != noSlot && orderOf[p.slot] != noSlot) {
                    px = orderOf[p.slot] < orderOf[c] ? w.lo : w.hi;
                } else if (p.slot != noSlot) {
                    px = cellX[p.slot] + p.offX;
                }
                lo = std::min(lo, px);
                hi = std::max(hi, px);
            }
            if (own != nullptr && lo <= hi) {
                bounds.insert(bounds.end(), {lo - own->offX, hi - own->offX});
            }
        }
        keepCentral(bounds, opts.clusterBounds);
        return bounds;
    };
    const auto place = [&](Cluster &cl) {
        double target = cellX[cells[cl.first]];
        if (!cl.bounds.empty()) {
            std::vector<double> scratch = cl.bounds;
            target = medianInterval(scratch).center();
        }
        cl.x = snapWithin(target, site, cl.width, w.lo, w.hi);
    };

    for (std::size_t k = w.first; k < w.last; ++k) {
        Cluster cur{boundsOf(cells[k]), cellW[cells[k]], 0.0, k};
        place(cur);
        while (!clusters.empty() && clusters.back().x + clusters.back().width > cur.x + eps) {
            Cluster prev = std::move(clusters.back());
            clusters.pop_back();
            for (const double b : cur.bounds) {
                prev.bounds.push_back(b - prev.width);
            }
            keepCentral(prev.bounds, opts.clusterBounds);
            prev.width += cur.width;
            place(prev);
            cur = std::move(prev);
        }
        clusters.push_back(std::move(cur));
    }

    placed.clear();
    for (std::size_t m = 0; m < clusters.size(); ++m) {
        const std::size_t end = m + 1 < clusters.size() ? clusters[m + 1].first : w.last;
        double x = clusters[m].x;
        for (std::size_t k = clusters[m].first; k < end; ++k) {
            placed.push_back(Move{cells[k], x, cellY[cells[k]]});
            x += cellW[cells[k]];
        }
    }
    bool overlapping = false;
    for (std::size_t k = w.first + 1; k < w.last; ++k) {
        overlapping =
            overlapping || cellX[cells[k - 1]] + cellW[cells[k - 1]] > cellX[cells[k]] + eps;
    }
    const bool fits = placed.back().x + cellW[cells[w.last - 1]] <= w.hi + eps;
    if (fits && (overlapping || wirelengthDelta(placed) < -eps)) {
        for (const Move &m : placed) {
            moves += std::abs(cellX[m.cell] - m.x) > eps ? 1 : 0;
            cellX[m.cell] = m.x;
        }
    }
    for (std::size_t k = w.first; k < w.last; ++k) {
        orderOf[cells[k]] = noSlot;
    }
    return moves;
}

void FastDetailedPlacer::Impl::writeFrame(const std::string &note, std::size_t step) const {
    if (!design.onFrame) {
        return;
    }
    const std::size_t nv = design.cells.size();
    std::vector<float> fx(nv);
    std::vector<float> fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(design.cells[v].x);
        fy[v] = static_cast<float>(design.cells[v].y);
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        fx[mov[i]] = static_cast<float>(cellX[i]);
        fy[mov[i]] = static_cast<float>(cellY[i]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.hpwl = hpwl();
    info.hpwlInitial = hpwlInput;
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

void FastDetailedPlacer::Impl::selfCheck(FastdpResult &res) const {
    res.overlappingPairs = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    for (std::size_t i = 0; i < mov.size(); ++i) {
        if (spanOf[i] == noSlot) {
            ++res.offRow;
            continue;
        }
        const Span &sp = spans[spanOf[i]];
        if (!sp.contains(cellX[i], cellW[i])) {
            ++res.offRow;
            continue;
        }
        const double q = cellX[i] / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > eps * std::max(1.0, std::fabs(cellX[i]))) {
            ++res.offSite;
        }
    }
    std::vector<std::size_t> all(mov.size());
    std::iota(all.begin(), all.end(), 0u);
    std::ranges::stable_sort(all, [&](std::size_t a, std::size_t b) {
        return cellY[a] != cellY[b] ? cellY[a] < cellY[b] : cellX[a] < cellX[b];
    });
    for (std::size_t k = 1; k < all.size(); ++k) {
        const std::size_t a = all[k - 1];
        const std::size_t b = all[k];
        if (cellY[a] == cellY[b] && cellX[a] + cellW[a] > cellX[b] + eps) {
            ++res.overlappingPairs;
        }
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        const long b = std::clamp<long>(static_cast<long>(std::floor((cellY[i] - bandY0) / bandY)),
                                        0, static_cast<long>(fixedByBand.size()) - 1);
        for (const std::size_t fi : fixedByBand[static_cast<std::size_t>(b)]) {
            const FixedBox &f = fixedBoxes[fi];
            if (cellX[i] + cellW[i] > f.x0 + eps && cellX[i] < f.x1 - eps &&
                cellY[i] + cellH[i] > f.y0 + eps && cellY[i] < f.y1 - eps) {
                ++res.overFixed;
                break;
            }
        }
    }
}

FastDetailedPlacer::Impl::Snapshot FastDetailedPlacer::Impl::snapshot() const {
    return {.x = cellX, .y = cellY, .spanOf = spanOf, .spans = spans, .used = used};
}

void FastDetailedPlacer::Impl::restore(Snapshot saved) {
    cellX = std::move(saved.x);
    cellY = std::move(saved.y);
    spanOf = std::move(saved.spanOf);
    spans = std::move(saved.spans);
    used = std::move(saved.used);
}

FastdpResult FastDetailedPlacer::Impl::place(const FastdpOptions &params) {
    FastdpResult res;
    ScopedTimer timer("fastdp");
    opts = params;
    mov.clear();
    cellW.clear();
    cellH.clear();
    cellX.clear();
    cellY.clear();
    cellRegion.clear();
    fixedBoxes.clear();
    tallSkipped = 0;
    bandY = inf;

    double rowHeight = inf;
    for (const RowInfo &ri : design.rows) {
        if (ri.height > 0.0) {
            rowHeight = std::min(rowHeight, ri.height);
        }
    }
    if (!std::isfinite(rowHeight)) {
        rowHeight = 1.0;
    }
    collectCells(rowHeight);
    if (mov.empty()) {
        return res;
    }
    buildSpans();
    indexFixed();
    buildNetlist();
    res.hpwlBefore = hpwl();
    hpwlInput = res.hpwlBefore;
    writeFrame("Input - legal placement");

    double globalSeconds = 0.0;
    double verticalSeconds = 0.0;
    double reorderSeconds = 0.0;
    double clusterSeconds = 0.0;
    const auto timed = [](double &total, auto &&technique) {
        const auto start = std::chrono::steady_clock::now();
        const std::size_t moves = technique();
        total += secondsSince(start);
        return moves;
    };
    const auto improvedEnough = [&](double before) {
        return before > 0.0 && (before - hpwl()) / before >= opts.minImprovement;
    };
    const auto cluster = [&] {
        res.clusterMoves += timed(clusterSeconds, [&] {
            return clusterSegments();
        });
    };
    const auto keptBetter = [&](Snapshot saved, double before, std::string_view what) {
        if (hpwl() <= before) {
            return true;
        }
        restore(std::move(saved));
        ++res.rolledBack;
        ktlog.echo("{} raised HPWL; rolled back to {:.6e}", what, before);
        return false;
    };

    std::size_t step = 0;
    if (opts.clustering) {
        cluster();
    }
    std::size_t weakPasses = 0;
    for (std::size_t iteration = 0; iteration < opts.maxIterations; ++iteration) {
        const double before = hpwl();
        Snapshot saved = snapshot();
        if (opts.globalSwap) {
            res.globalSwaps += timed(globalSeconds, [&] {
                return keepShorter(&Impl::globalSwap);
            });
            writeFrame(std::format("Global swap - pass {}", iteration + 1), ++step);
        }
        if (opts.verticalSwap) {
            res.verticalSwaps += timed(verticalSeconds, [&] {
                return keepShorter(&Impl::verticalSwap);
            });
            writeFrame(std::format("Vertical swap - pass {}", iteration + 1), ++step);
        }
        if (opts.localReorder) {
            res.reorderMoves += timed(reorderSeconds, [&] {
                return localReorder();
            });
            writeFrame(std::format("Local reordering - pass {}", iteration + 1), ++step);
        }
        if (opts.clustering) {
            cluster();
        }
        if (!keptBetter(std::move(saved), before, std::format("pass {}", iteration + 1))) {
            break;
        }
        weakPasses = improvedEnough(before) ? 0 : weakPasses + 1;
        if (weakPasses == opts.weakPassLimit) {
            break;
        }
    }
    if (opts.clustering) {
        for (std::size_t iteration = 0; iteration < opts.maxIterations; ++iteration) {
            const double before = hpwl();
            Snapshot saved = snapshot();
            cluster();
            writeFrame(std::format("Single-segment clustering - round {}", iteration + 1), ++step);
            if (!keptBetter(std::move(saved), before,
                            std::format("clustering round {}", iteration + 1)) ||
                !improvedEnough(before)) {
                break;
            }
        }
    }
    res.globalSeconds = globalSeconds;
    res.verticalSeconds = verticalSeconds;
    res.reorderSeconds = reorderSeconds;
    res.clusterSeconds = clusterSeconds;

    res.hpwlAfter = hpwl();
    selfCheck(res);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        design.cells[mov[i]].x = cellX[i];
        design.cells[mov[i]].y = cellY[i];
    }
    res.seconds = timer.elapsedSeconds();
    if (tallSkipped > 0) {
        ktlog.echo("left {} multi-row cell(s) where legalization put them", tallSkipped);
    }
    return res;
}

FastDetailedPlacer::FastDetailedPlacer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

FastDetailedPlacer::~FastDetailedPlacer() = default;

FastdpResult FastDetailedPlacer::place(const FastdpOptions &options) {
    return pImpl->place(options);
}

}  // namespace ktplace::fastdp
