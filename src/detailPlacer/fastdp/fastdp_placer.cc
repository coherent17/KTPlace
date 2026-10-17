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
#include <limits>
#include <memory>
#include <numeric>
#include <span>
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
    struct Candidate {
        double benefit = 0.0;
        std::size_t partner = noSlot;
        std::size_t span = noSlot;
        double x = 0.0;
        double partnerX = 0.0;
    };

    void collectCells(double rowHeight);
    void buildSpans();
    void assignCells();
    void indexFixed();
    void buildNetlist();

    [[nodiscard]] double hpwl() const;
    [[nodiscard]] double wirelengthDelta(std::span<const Move> moves) const;
    [[nodiscard]] OptimalRegion optimalRegion(std::size_t c) const;

    [[nodiscard]] std::size_t indexIn(std::size_t s, std::size_t c) const;
    [[nodiscard]] double gap(std::size_t s, std::ptrdiff_t g) const;
    [[nodiscard]] double snap(double x, std::size_t s, double w) const;
    [[nodiscard]] double penalty(double excess, double nearRoom, double wideRoom) const;
    [[nodiscard]] double cellPenalty(std::size_t s, std::size_t k, double excess) const;
    [[nodiscard]] std::size_t nearestRow(double y) const;

    [[nodiscard]] Candidate swapWith(std::size_t i, std::size_t j) const;
    [[nodiscard]] Candidate moveInto(std::size_t i, std::size_t s, std::size_t g,
                                     double target) const;
    [[nodiscard]] bool tryGlobal(std::size_t i, const OptimalRegion &region);
    void apply(std::size_t i, const Candidate &c);
    void relocate(std::size_t c, double nx, std::size_t s);
    void legalizeSpan(std::size_t s);
    void legalizeTouched();

    std::size_t globalSwap();
    std::size_t verticalSwap();
    std::size_t localReorder();
    std::size_t clusterSegments();

    void selfCheck(FastdpResult &res) const;
    void writeFrame(const char *note, std::size_t step = 0) const;
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
    std::vector<char> touched;
    std::vector<std::vector<std::size_t>> spansInRow;
    std::vector<double> rowY;
    std::vector<std::vector<NetPin>> netPins;
    std::vector<std::vector<std::size_t>> cellNets;
    std::vector<FixedBox> fixedBoxes;
    std::vector<std::vector<std::size_t>> fixedByBand;
    double bandY = inf;
    double bandY0 = 0.0;
    std::size_t tallSkipped = 0;

    mutable std::vector<std::size_t> moveOf;
    mutable std::vector<std::uint32_t> netStamp;
    mutable std::uint32_t stamp = 0;
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
    touched.assign(spans.size(), 0);
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
    moveOf.assign(mov.size(), noSlot);
    netStamp.assign(design.nets.size(), 0);
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

double FastDetailedPlacer::Impl::wirelengthDelta(std::span<const Move> moves) const {
    if (++stamp == 0) {
        std::ranges::fill(netStamp, 0u);
        stamp = 1;
    }
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
    const Span &sp = spans[s];
    const double lo = std::ceil(sp.xlo / sp.site - eps) * sp.site;
    const double hi = std::floor((sp.xhi - w) / sp.site + eps) * sp.site;
    return std::clamp(std::round(x / sp.site) * sp.site, lo, std::max(lo, hi));
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

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::swapWith(std::size_t i,
                                                                       std::size_t j) const {
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
    return {-wirelengthDelta(moves) - overlap, j, sj, xi, xj};
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::moveInto(std::size_t i, std::size_t s,
                                                                       std::size_t g,
                                                                       double target) const {
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
    return {-wirelengthDelta(moves) - overlap, noSlot, s, x, 0.0};
}

bool FastDetailedPlacer::Impl::tryGlobal(std::size_t i, const OptimalRegion &region) {
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
    Candidate best;
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
                const Candidate c =
                    item.isGap ? moveInto(i, s, item.index, cx) : swapWith(i, cells[item.index]);
                if (c.span != noSlot && c.benefit > best.benefit) {
                    best = c;
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
    if (best.span == noSlot || best.benefit <= eps) {
        return false;
    }
    apply(i, best);
    return true;
}

void FastDetailedPlacer::Impl::apply(std::size_t i, const Candidate &c) {
    const std::size_t si = spanOf[i];
    if (c.partner != noSlot) {
        relocate(c.partner, c.partnerX, si);
    }
    relocate(i, c.x, c.span);
}

void FastDetailedPlacer::Impl::relocate(std::size_t c, double nx, std::size_t s) {
    if (const std::size_t old = spanOf[c]; old != noSlot) {
        std::vector<std::size_t> &cells = spans[old].cells;
        cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(indexIn(old, c)));
        used[old] -= cellW[c];
        touched[old] = 1;
    }
    cellX[c] = nx;
    cellY[c] = spans[s].ylo;
    std::vector<std::size_t> &cells = spans[s].cells;
    cells.insert(std::ranges::upper_bound(cells, nx, {},
                                          [this](std::size_t a) {
                                              return cellX[a];
                                          }),
                 c);
    used[s] += cellW[c];
    spanOf[c] = s;
    touched[s] = 1;
}

void FastDetailedPlacer::Impl::legalizeSpan(std::size_t s) {
    Span &sp = spans[s];
    if (sp.cells.empty() || used[s] > sp.width() + eps) {
        return;
    }
    std::ranges::stable_sort(sp.cells, {}, [this](std::size_t c) {
        return cellX[c];
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
        cl.x = snap(cl.q / cl.weight, s, cl.width);
    };
    for (std::size_t k = 0; k < sp.cells.size(); ++k) {
        const std::size_t c = sp.cells[k];
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
        const std::size_t end = m + 1 < clusters.size() ? clusters[m + 1].first : sp.cells.size();
        double x = clusters[m].x;
        for (std::size_t k = clusters[m].first; k < end; ++k) {
            cellX[sp.cells[k]] = x;
            x += cellW[sp.cells[k]];
        }
    }
}

void FastDetailedPlacer::Impl::legalizeTouched() {
    for (std::size_t s = 0; s < spans.size(); ++s) {
        if (touched[s]) {
            legalizeSpan(s);
            touched[s] = 0;
        }
    }
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    std::size_t swaps = 0;
    for (std::size_t i = 0; i < mov.size(); ++i) {
        if (spanOf[i] == noSlot) {
            continue;
        }
        const OptimalRegion region = optimalRegion(i);
        if (region.x.contains(cellX[i]) && region.y.contains(cellY[i])) {
            continue;
        }
        swaps += tryGlobal(i, region) ? 1 : 0;
    }
    legalizeTouched();
    return swaps;
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    std::size_t swaps = 0;
    for (std::size_t i = 0; i < mov.size(); ++i) {
        const std::size_t si = spanOf[i];
        if (si == noSlot) {
            continue;
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
            continue;
        }
        Candidate best;
        for (const std::size_t s : spansInRow[target]) {
            const Span &sp = spans[s];
            if (sp.region != cellRegion[i] || sp.cells.empty() || sp.xhi < cellX[i] ||
                sp.xlo > cellX[i] + cellW[i]) {
                continue;
            }
            const auto at =
                static_cast<std::size_t>(std::ranges::lower_bound(sp.cells, cellX[i], {},
                                                                  [this](std::size_t c) {
                                                                      return cellX[c];
                                                                  }) -
                                         sp.cells.begin());
            const std::size_t reach = opts.verticalCandidates;
            const std::size_t first = at > reach ? at - reach : 0;
            const std::size_t last = std::min(sp.cells.size(), at + reach);
            for (std::size_t k = first; k < last; ++k) {
                const Candidate c = swapWith(i, sp.cells[k]);
                if (c.span != noSlot && c.benefit > best.benefit) {
                    best = c;
                }
            }
        }
        if (best.span != noSlot && best.benefit > eps) {
            apply(i, best);
            ++swaps;
        }
    }
    legalizeTouched();
    return swaps;
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
    struct Cluster {
        std::vector<double> bounds;
        double width;
        double x;
        std::size_t first;
    };
    std::size_t moves = 0;
    std::vector<std::size_t> orderOf(mov.size(), noSlot);
    std::vector<Move> placed;
    std::vector<Cluster> clusters;
    for (std::size_t s = 0; s < spans.size(); ++s) {
        const Span &sp = spans[s];
        const std::vector<std::size_t> &cells = sp.cells;
        if (cells.empty()) {
            continue;
        }
        for (std::size_t k = 0; k < cells.size(); ++k) {
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
                    if (p.slot != noSlot && spanOf[p.slot] == s) {
                        px = orderOf[p.slot] < orderOf[c] ? sp.xlo : sp.xhi;
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
            cl.x = snap(target, s, cl.width);
        };

        clusters.clear();
        for (std::size_t k = 0; k < cells.size(); ++k) {
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
            const std::size_t end = m + 1 < clusters.size() ? clusters[m + 1].first : cells.size();
            double x = clusters[m].x;
            for (std::size_t k = clusters[m].first; k < end; ++k) {
                placed.push_back(Move{cells[k], x, cellY[cells[k]]});
                x += cellW[cells[k]];
            }
        }
        bool overlapping = false;
        for (std::size_t k = 1; k < cells.size(); ++k) {
            overlapping =
                overlapping || cellX[cells[k - 1]] + cellW[cells[k - 1]] > cellX[cells[k]] + eps;
        }
        const bool fits = placed.back().x + cellW[cells.back()] <= sp.xhi + eps;
        if (fits && (overlapping || wirelengthDelta(placed) < -eps)) {
            for (const Move &m : placed) {
                moves += std::abs(cellX[m.cell] - m.x) > eps ? 1 : 0;
                cellX[m.cell] = m.x;
            }
        }
        for (const std::size_t c : cells) {
            orderOf[c] = noSlot;
        }
    }
    return moves;
}

void FastDetailedPlacer::Impl::writeFrame(const char *note, std::size_t step) const {
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
    writeFrame("legalized input");

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

    std::size_t step = 0;
    if (opts.clustering) {
        cluster();
    }
    for (std::size_t iteration = 0; iteration < opts.maxIterations; ++iteration) {
        const double before = hpwl();
        if (opts.globalSwap) {
            res.globalSwaps += timed(globalSeconds, [&] {
                return globalSwap();
            });
            writeFrame("global swap", ++step);
        }
        if (opts.verticalSwap) {
            res.verticalSwaps += timed(verticalSeconds, [&] {
                return verticalSwap();
            });
            writeFrame("vertical swap", ++step);
        }
        if (opts.localReorder) {
            res.reorderMoves += timed(reorderSeconds, [&] {
                return localReorder();
            });
            writeFrame("local re-ordering", ++step);
        }
        if (!improvedEnough(before)) {
            break;
        }
    }
    if (opts.clustering) {
        for (std::size_t iteration = 0; iteration < opts.maxIterations; ++iteration) {
            const double before = hpwl();
            cluster();
            writeFrame("single-segment clustering", ++step);
            if (!improvedEnough(before)) {
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
