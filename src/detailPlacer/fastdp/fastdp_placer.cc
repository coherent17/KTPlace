// @file fastdp_placer.cc
// FastDP detailed placement (Pan, Viswanathan and Chu, ICCAD 2005)

#include "detailPlacer/fastdp/fastdp_placer.h"

#include "detailPlacer/fastdp/fastdp_design.h"
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

namespace ktplace {

namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();
constexpr double kEps = 1e-6;
constexpr double kInf = std::numeric_limits<double>::infinity();

struct Span {
    double ylo = 0.0;
    double yhi = 0.0;
    double xlo = 0.0;
    double xhi = 0.0;
    double site = 1.0;
    std::size_t row = 0;
    int region = fastdp::kNoRegion;
    std::vector<std::size_t> cells;

    [[nodiscard]] double width() const noexcept {
        return xhi - xlo;
    }
    [[nodiscard]] bool contains(double px, double pw) const noexcept {
        return px >= xlo - kEps && px + pw <= xhi + kEps;
    }
};

struct NetPin {
    std::size_t slot = kNoSlot;
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
        return v >= lo - kEps && v <= hi + kEps;
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
    std::size_t cell = kNoSlot;
    double x = 0.0;
    double y = 0.0;
};

struct BoundingBox {
    double xlo = kInf;
    double xhi = -kInf;
    double ylo = kInf;
    double yhi = -kInf;

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
    std::ranges::sort(bounds);
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
    explicit Impl(const fastdp::Design &d) : design(d) {}

    DetailPlaceResult place(const FastdpOptions &params);

    const fastdp::Design &design;
    std::vector<std::size_t> mov_;
    std::vector<double> x_;
    std::vector<double> y_;

private:
    struct Candidate {
        double benefit = 0.0;
        std::size_t partner = kNoSlot;
        std::size_t span = kNoSlot;
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

    void selfCheck(DetailPlaceResult &res) const;
    void writeFrame(const char *note, std::size_t step = 0) const;

    FastdpOptions opts_;
    std::vector<double> w_;
    std::vector<double> h_;
    std::vector<int> region_;
    std::vector<char> movable_;
    std::vector<char> stranded_;
    std::vector<std::size_t> spanOf_;
    std::vector<Span> spans_;
    std::vector<double> used_;
    std::vector<char> touched_;
    std::vector<std::vector<std::size_t>> spansInRow_;
    std::vector<double> rowY_;
    std::vector<std::vector<NetPin>> netPins_;
    std::vector<std::vector<std::size_t>> cellNets_;
    std::vector<FixedBox> fixed_;
    std::vector<std::vector<std::size_t>> fixedByBand_;
    double bandY_ = kInf;
    double bandY0_ = 0.0;
    std::size_t tallSkipped_ = 0;

    mutable std::vector<std::size_t> moveOf_;
    mutable std::vector<std::uint32_t> netStamp_;
    mutable std::uint32_t stamp_ = 0;
};

void FastDetailedPlacer::Impl::collectCells(double rowHeight) {
    movable_.assign(design.cells.size(), 0);
    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        const fastdp::Cell &cell = design.cells[v];
        const bool tall = cell.height > rowHeight + kEps;
        if (cell.isFixed || tall) {
            fixed_.push_back(FixedBox{cell.y, cell.y + cell.height, cell.x, cell.x + cell.width});
        }
        if (cell.isFixed || cell.isTerminal || tall) {
            tallSkipped_ += (tall && !cell.isFixed && !cell.isTerminal) ? 1 : 0;
            continue;
        }
        movable_[v] = 1;
        mov_.push_back(v);
        w_.push_back(cell.width);
        h_.push_back(cell.height);
        x_.push_back(cell.x);
        y_.push_back(cell.y);
        region_.push_back(cell.region);
    }
}

void FastDetailedPlacer::Impl::buildSpans() {
    std::vector<fastdp::RowInfo> rows(design.rows);
    std::ranges::sort(rows, {}, &fastdp::RowInfo::coordinate);
    rowY_.clear();
    for (const fastdp::RowInfo &ri : rows) {
        rowY_.push_back(ri.coordinate);
    }
    stranded_.assign(mov_.size(), 0);
    for (int attempt = 0; attempt < 2; ++attempt) {
        std::vector<std::vector<std::pair<double, double>>> blocked(rows.size());
        const auto block = [&](double x0, double x1, double y0, double y1) {
            auto r = std::ranges::upper_bound(rows, y0 + kEps, {}, [](const fastdp::RowInfo &ri) {
                return ri.coordinate + ri.height;
            });
            for (; r != rows.end() && r->coordinate < y1 - kEps; ++r) {
                blocked[static_cast<std::size_t>(r - rows.begin())].emplace_back(x0, x1);
            }
        };
        for (std::size_t v = 0; v < design.cells.size(); ++v) {
            const fastdp::Cell &cell = design.cells[v];
            if (!movable_[v] && cell.width > 0.0 && cell.height > 0.0) {
                block(cell.x, cell.x + cell.width, cell.y, cell.y + cell.height);
            }
        }
        for (std::size_t i = 0; i < mov_.size(); ++i) {
            if (stranded_[i]) {
                block(x_[i], x_[i] + w_[i], y_[i], y_[i] + h_[i]);
            }
        }

        spans_.clear();
        spansInRow_.assign(rows.size(), {});
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const fastdp::RowInfo &ri = rows[r];
            std::vector<std::pair<double, double>> &obs = blocked[r];
            std::ranges::sort(obs);
            for (const fastdp::Subrow &sub : ri.subrows) {
                const auto emit = [&](double lo, double hi) {
                    if (hi > lo + kEps) {
                        spansInRow_[r].push_back(spans_.size());
                        spans_.push_back(Span{ri.coordinate,
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
                    if (o1 <= cursor + kEps || o0 >= sub.xhi - kEps) {
                        continue;
                    }
                    emit(cursor, std::min(o0, sub.xhi));
                    cursor = std::max(cursor, o1);
                }
                emit(cursor, sub.xhi);
            }
        }
        assignCells();
        if (attempt == 1 || std::ranges::none_of(spanOf_, [](std::size_t s) {
                return s == kNoSlot;
            })) {
            break;
        }
        for (std::size_t i = 0; i < mov_.size(); ++i) {
            stranded_[i] = spanOf_[i] == kNoSlot ? 1 : 0;
        }
    }
    used_.assign(spans_.size(), 0.0);
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        for (const std::size_t c : spans_[s].cells) {
            used_[s] += w_[c];
        }
    }
    touched_.assign(spans_.size(), 0);
}

void FastDetailedPlacer::Impl::assignCells() {
    spanOf_.assign(mov_.size(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        if (stranded_[i]) {
            continue;
        }
        const std::size_t r = nearestRow(y_[i]);
        if (r == kNoSlot || std::abs(rowY_[r] - y_[i]) > kEps) {
            continue;
        }
        for (const std::size_t s : spansInRow_[r]) {
            if (spans_[s].region == region_[i] && spans_[s].contains(x_[i], w_[i])) {
                spanOf_[i] = s;
                spans_[s].cells.push_back(i);
                break;
            }
        }
    }
    for (Span &sp : spans_) {
        std::ranges::sort(sp.cells, {}, [this](std::size_t c) {
            return x_[c];
        });
    }
}

void FastDetailedPlacer::Impl::indexFixed() {
    bandY0_ = rowY_.empty() ? 0.0 : rowY_.front();
    for (const fastdp::RowInfo &ri : design.rows) {
        if (ri.height > 0.0) {
            bandY_ = std::min(bandY_, ri.height);
        }
    }
    if (!std::isfinite(bandY_)) {
        bandY_ = 1.0;
    }
    const std::size_t bands = design.rows.size() + 2;
    fixedByBand_.assign(bands, {});
    const auto band = [&](double y) {
        return std::clamp<long>(static_cast<long>(std::floor((y - bandY0_) / bandY_)), 0,
                                static_cast<long>(bands) - 1);
    };
    for (std::size_t i = 0; i < fixed_.size(); ++i) {
        for (long b = band(fixed_[i].y0); b <= band(fixed_[i].y1); ++b) {
            fixedByBand_[static_cast<std::size_t>(b)].push_back(i);
        }
    }
}

void FastDetailedPlacer::Impl::buildNetlist() {
    std::vector<std::size_t> slotOf(design.cells.size(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        slotOf[mov_[i]] = i;
    }
    netPins_.assign(design.nets.size(), {});
    cellNets_.assign(mov_.size(), {});
    for (std::size_t n = 0; n < design.nets.size(); ++n) {
        for (const std::size_t pinId : design.nets[n].pins) {
            const fastdp::Pin &pin = design.pins[pinId];
            const std::size_t s = slotOf[pin.cellId];
            const fastdp::Cell &c = design.cells[pin.cellId];
            if (s == kNoSlot) {
                netPins_[n].push_back(NetPin{kNoSlot, pin.offsetX, pin.offsetY, c.x + pin.offsetX,
                                             c.y + pin.offsetY});
                continue;
            }
            netPins_[n].push_back(NetPin{s, pin.offsetX, pin.offsetY, 0.0, 0.0});
            if (cellNets_[s].empty() || cellNets_[s].back() != n) {
                cellNets_[s].push_back(n);
            }
        }
    }
    moveOf_.assign(mov_.size(), kNoSlot);
    netStamp_.assign(design.nets.size(), 0);
}

double FastDetailedPlacer::Impl::hpwl() const {
    double total = 0.0;
    for (const std::vector<NetPin> &pins : netPins_) {
        BoundingBox box;
        for (const NetPin &p : pins) {
            if (p.slot == kNoSlot) {
                box.add(p.absX, p.absY);
            } else {
                box.add(x_[p.slot] + p.offX, y_[p.slot] + p.offY);
            }
        }
        total += box.halfPerimeter();
    }
    return total;
}

double FastDetailedPlacer::Impl::wirelengthDelta(std::span<const Move> moves) const {
    if (++stamp_ == 0) {
        std::ranges::fill(netStamp_, 0u);
        stamp_ = 1;
    }
    for (std::size_t k = 0; k < moves.size(); ++k) {
        moveOf_[moves[k].cell] = k;
    }
    double delta = 0.0;
    for (const Move &m : moves) {
        for (const std::size_t n : cellNets_[m.cell]) {
            if (netStamp_[n] == stamp_) {
                continue;
            }
            netStamp_[n] = stamp_;
            BoundingBox before;
            BoundingBox after;
            for (const NetPin &p : netPins_[n]) {
                if (p.slot == kNoSlot) {
                    before.add(p.absX, p.absY);
                    after.add(p.absX, p.absY);
                    continue;
                }
                before.add(x_[p.slot] + p.offX, y_[p.slot] + p.offY);
                if (const std::size_t k = moveOf_[p.slot]; k != kNoSlot) {
                    after.add(moves[k].x + p.offX, moves[k].y + p.offY);
                } else {
                    after.add(x_[p.slot] + p.offX, y_[p.slot] + p.offY);
                }
            }
            delta += after.halfPerimeter() - before.halfPerimeter();
        }
    }
    for (const Move &m : moves) {
        moveOf_[m.cell] = kNoSlot;
    }
    return delta;
}

OptimalRegion FastDetailedPlacer::Impl::optimalRegion(std::size_t c) const {
    std::vector<double> xs;
    std::vector<double> ys;
    for (const std::size_t n : cellNets_[c]) {
        BoundingBox others;
        const NetPin *own = nullptr;
        for (const NetPin &p : netPins_[n]) {
            if (p.slot == c) {
                own = own != nullptr ? own : &p;
            } else if (p.slot == kNoSlot) {
                others.add(p.absX, p.absY);
            } else {
                others.add(x_[p.slot] + p.offX, y_[p.slot] + p.offY);
            }
        }
        if (own == nullptr || others.xlo > others.xhi) {
            continue;
        }
        xs.insert(xs.end(), {others.xlo - own->offX, others.xhi - own->offX});
        ys.insert(ys.end(), {others.ylo - own->offY, others.yhi - own->offY});
    }
    if (xs.empty()) {
        return {{x_[c], x_[c]}, {y_[c], y_[c]}};
    }
    return {medianInterval(xs), medianInterval(ys)};
}

std::size_t FastDetailedPlacer::Impl::indexIn(std::size_t s, std::size_t c) const {
    const std::vector<std::size_t> &cells = spans_[s].cells;
    auto it = std::ranges::lower_bound(cells, x_[c] - kEps, {}, [this](std::size_t a) {
        return x_[a];
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
    const Span &sp = spans_[s];
    const auto n = static_cast<std::ptrdiff_t>(sp.cells.size());
    if (g < 0 || g > n) {
        return 0.0;
    }
    double left = sp.xlo;
    if (g > 0) {
        const std::size_t prev = sp.cells[static_cast<std::size_t>(g - 1)];
        left = x_[prev] + w_[prev];
    }
    const double right = g == n ? sp.xhi : x_[sp.cells[static_cast<std::size_t>(g)]];
    return std::max(0.0, right - left);
}

double FastDetailedPlacer::Impl::snap(double x, std::size_t s, double w) const {
    const Span &sp = spans_[s];
    const double lo = std::ceil(sp.xlo / sp.site - kEps) * sp.site;
    const double hi = std::floor((sp.xhi - w) / sp.site + kEps) * sp.site;
    return std::clamp(std::round(x / sp.site) * sp.site, lo, std::max(lo, hi));
}

double FastDetailedPlacer::Impl::penalty(double excess, double nearRoom, double wideRoom) const {
    return std::max(0.0, excess - nearRoom) * opts_.penaltyWeight1 +
           std::max(0.0, excess - wideRoom) * opts_.penaltyWeight2;
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
    if (rowY_.empty()) {
        return kNoSlot;
    }
    const auto it = std::ranges::lower_bound(rowY_, y);
    if (it == rowY_.end()) {
        return rowY_.size() - 1;
    }
    if (it != rowY_.begin() && y - *std::prev(it) < *it - y) {
        return static_cast<std::size_t>(it - rowY_.begin()) - 1;
    }
    return static_cast<std::size_t>(it - rowY_.begin());
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::swapWith(std::size_t i,
                                                                       std::size_t j) const {
    const std::size_t si = spanOf_[i];
    const std::size_t sj = spanOf_[j];
    if (j == i || sj == kNoSlot || spans_[si].region != region_[j]) {
        return {};
    }
    if (si != sj && (used_[sj] - w_[j] + w_[i] > spans_[sj].width() + kEps ||
                     used_[si] - w_[i] + w_[j] > spans_[si].width() + kEps)) {
        return {};
    }
    const double xi = snap(x_[j] + 0.5 * (w_[j] - w_[i]), sj, w_[i]);
    const double xj = snap(x_[i] + 0.5 * (w_[i] - w_[j]), si, w_[j]);
    const std::array<Move, 2> moves{Move{i, xi, spans_[sj].ylo}, Move{j, xj, spans_[si].ylo}};
    const double overlap = w_[i] > w_[j] ? cellPenalty(sj, indexIn(sj, j), w_[i] - w_[j])
                                         : cellPenalty(si, indexIn(si, i), w_[j] - w_[i]);
    return {-wirelengthDelta(moves) - overlap, j, sj, xi, xj};
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::moveInto(std::size_t i, std::size_t s,
                                                                       std::size_t g,
                                                                       double target) const {
    if (s != spanOf_[i] && used_[s] + w_[i] > spans_[s].width() + kEps) {
        return {};
    }
    const Span &sp = spans_[s];
    const auto gi = static_cast<std::ptrdiff_t>(g);
    const double room = gap(s, gi);
    const double left = g == 0 ? sp.xlo : x_[sp.cells[g - 1]] + w_[sp.cells[g - 1]];
    const double x = room >= w_[i] ? snap(std::clamp(target, left, left + room - w_[i]), s, w_[i])
                                   : snap(left + 0.5 * (room - w_[i]), s, w_[i]);
    const std::array<Move, 1> moves{Move{i, x, sp.ylo}};
    const double overlap =
        room >= w_[i] ? 0.0 : penalty(w_[i], room, room + gap(s, gi - 1) + gap(s, gi + 1));
    return {-wirelengthDelta(moves) - overlap, kNoSlot, s, x, 0.0};
}

bool FastDetailedPlacer::Impl::tryGlobal(std::size_t i, const OptimalRegion &region) {
    const double cx = region.x.center();
    const double cy = region.y.center();
    std::vector<std::size_t> rows;
    const auto lo = std::ranges::lower_bound(rowY_, region.y.lo - kEps);
    const auto hi = std::ranges::upper_bound(rowY_, region.y.hi + kEps);
    for (auto r = lo; r != hi; ++r) {
        rows.push_back(static_cast<std::size_t>(r - rowY_.begin()));
    }
    if (rows.empty()) {
        rows.push_back(nearestRow(cy));
    }
    std::ranges::sort(rows, {}, [&](std::size_t r) {
        return std::abs(rowY_[r] - cy);
    });

    struct Item {
        double distance;
        std::size_t index;
        bool isGap;
    };
    std::vector<Item> items;
    std::size_t budget = opts_.swapCandidates;
    Candidate best;
    for (const std::size_t r : rows) {
        for (const std::size_t s : spansInRow_[r]) {
            const Span &sp = spans_[s];
            if (sp.region != region_[i] || sp.xhi < region.x.lo || sp.xlo > region.x.hi + w_[i]) {
                continue;
            }
            const std::vector<std::size_t> &cells = sp.cells;
            const auto at =
                static_cast<std::size_t>(std::ranges::lower_bound(cells, cx, {},
                                                                  [this](std::size_t c) {
                                                                      return x_[c];
                                                                  }) -
                                         cells.begin());
            const std::size_t first = at > budget ? at - budget : 0;
            const std::size_t last = std::min(cells.size(), at + budget);
            items.clear();
            for (std::size_t k = first; k < last; ++k) {
                if (region.x.contains(x_[cells[k]])) {
                    items.push_back({std::abs(x_[cells[k]] - cx), k, false});
                }
            }
            for (std::size_t g = first; g <= last; ++g) {
                const double left = g == 0 ? sp.xlo : x_[cells[g - 1]] + w_[cells[g - 1]];
                const double right = g == cells.size() ? sp.xhi : x_[cells[g]];
                if (right > left + kEps && right >= region.x.lo - kEps &&
                    left <= region.x.hi + kEps) {
                    items.push_back({std::abs(std::clamp(cx, left, right) - cx), g, true});
                }
            }
            std::ranges::sort(items, {}, &Item::distance);
            for (const Item &item : items) {
                const Candidate c =
                    item.isGap ? moveInto(i, s, item.index, cx) : swapWith(i, cells[item.index]);
                if (c.span != kNoSlot && c.benefit > best.benefit) {
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
    if (best.span == kNoSlot || best.benefit <= kEps) {
        return false;
    }
    apply(i, best);
    return true;
}

void FastDetailedPlacer::Impl::apply(std::size_t i, const Candidate &c) {
    const std::size_t si = spanOf_[i];
    if (c.partner != kNoSlot) {
        relocate(c.partner, c.partnerX, si);
    }
    relocate(i, c.x, c.span);
}

void FastDetailedPlacer::Impl::relocate(std::size_t c, double nx, std::size_t s) {
    if (const std::size_t old = spanOf_[c]; old != kNoSlot) {
        std::vector<std::size_t> &cells = spans_[old].cells;
        cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(indexIn(old, c)));
        used_[old] -= w_[c];
        touched_[old] = 1;
    }
    x_[c] = nx;
    y_[c] = spans_[s].ylo;
    std::vector<std::size_t> &cells = spans_[s].cells;
    cells.insert(std::ranges::upper_bound(cells, nx, {},
                                          [this](std::size_t a) {
                                              return x_[a];
                                          }),
                 c);
    used_[s] += w_[c];
    spanOf_[c] = s;
    touched_[s] = 1;
}

void FastDetailedPlacer::Impl::legalizeSpan(std::size_t s) {
    Span &sp = spans_[s];
    if (sp.cells.empty() || used_[s] > sp.width() + kEps) {
        return;
    }
    std::ranges::stable_sort(sp.cells, {}, [this](std::size_t c) {
        return x_[c];
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
        Cluster cur{1.0, x_[c], w_[c], 0.0, k};
        place(cur);
        while (!clusters.empty() && clusters.back().x + clusters.back().width > cur.x + kEps) {
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
            x_[sp.cells[k]] = x;
            x += w_[sp.cells[k]];
        }
    }
}

void FastDetailedPlacer::Impl::legalizeTouched() {
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        if (touched_[s]) {
            legalizeSpan(s);
            touched_[s] = 0;
        }
    }
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    std::size_t swaps = 0;
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        if (spanOf_[i] == kNoSlot) {
            continue;
        }
        const OptimalRegion region = optimalRegion(i);
        if (region.x.contains(x_[i]) && region.y.contains(y_[i])) {
            continue;
        }
        swaps += tryGlobal(i, region) ? 1 : 0;
    }
    legalizeTouched();
    return swaps;
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    std::size_t swaps = 0;
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const std::size_t si = spanOf_[i];
        if (si == kNoSlot) {
            continue;
        }
        const OptimalRegion region = optimalRegion(i);
        const std::size_t row = spans_[si].row;
        std::size_t target = kNoSlot;
        if (region.y.lo > y_[i] + kEps && row + 1 < rowY_.size()) {
            target = row + 1;
        } else if (region.y.hi < y_[i] - kEps && row > 0) {
            target = row - 1;
        }
        if (target == kNoSlot) {
            continue;
        }
        Candidate best;
        for (const std::size_t s : spansInRow_[target]) {
            const Span &sp = spans_[s];
            if (sp.region != region_[i] || sp.cells.empty() || sp.xhi < x_[i] ||
                sp.xlo > x_[i] + w_[i]) {
                continue;
            }
            const auto at =
                static_cast<std::size_t>(std::ranges::lower_bound(sp.cells, x_[i], {},
                                                                  [this](std::size_t c) {
                                                                      return x_[c];
                                                                  }) -
                                         sp.cells.begin());
            const std::size_t reach = opts_.verticalCandidates;
            const std::size_t first = at > reach ? at - reach : 0;
            const std::size_t last = std::min(sp.cells.size(), at + reach);
            for (std::size_t k = first; k < last; ++k) {
                const Candidate c = swapWith(i, sp.cells[k]);
                if (c.span != kNoSlot && c.benefit > best.benefit) {
                    best = c;
                }
            }
        }
        if (best.span != kNoSlot && best.benefit > kEps) {
            apply(i, best);
            ++swaps;
        }
    }
    legalizeTouched();
    return swaps;
}

std::size_t FastDetailedPlacer::Impl::localReorder() {
    const std::size_t n = std::min<std::size_t>(opts_.reorderWindow, 6);
    if (n < 2) {
        return 0;
    }
    std::size_t moves = 0;
    std::vector<std::size_t> order(n);
    std::vector<Move> trial(n);
    std::vector<Move> best(n);
    for (Span &sp : spans_) {
        std::vector<std::size_t> &cells = sp.cells;
        for (std::size_t k = 0; k + n <= cells.size(); ++k) {
            const std::span<std::size_t> group(cells.data() + k, n);
            const double left = x_[group.front()];
            const double right = x_[group.back()] + w_[group.back()];
            double widths = 0.0;
            for (const std::size_t c : group) {
                widths += w_[c];
            }
            const double spacing = (right - left - widths) / static_cast<double>(n - 1);
            if (spacing < -kEps) {
                continue;
            }
            std::iota(order.begin(), order.end(), 0u);
            double bestDelta = -kEps;
            bool improved = false;
            do {
                double x = left;
                for (std::size_t t = 0; t < n; ++t) {
                    const std::size_t c = group[order[t]];
                    trial[t] =
                        Move{c, left + std::floor((x - left) / sp.site + kEps) * sp.site, y_[c]};
                    x += w_[c] + spacing;
                }
                if (const double delta = wirelengthDelta(trial); delta < bestDelta) {
                    bestDelta = delta;
                    best = trial;
                    improved = true;
                }
            } while (std::ranges::next_permutation(order).found);
            if (improved) {
                for (std::size_t t = 0; t < n; ++t) {
                    moves += std::abs(x_[best[t].cell] - best[t].x) > kEps ? 1 : 0;
                    x_[best[t].cell] = best[t].x;
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
    std::vector<std::size_t> orderOf(mov_.size(), kNoSlot);
    std::vector<Move> placed;
    std::vector<Cluster> clusters;
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        const Span &sp = spans_[s];
        const std::vector<std::size_t> &cells = sp.cells;
        if (cells.empty()) {
            continue;
        }
        for (std::size_t k = 0; k < cells.size(); ++k) {
            orderOf[cells[k]] = k;
        }
        const auto boundsOf = [&](std::size_t c) {
            std::vector<double> bounds;
            for (const std::size_t n : cellNets_[c]) {
                double lo = kInf;
                double hi = -kInf;
                const NetPin *own = nullptr;
                for (const NetPin &p : netPins_[n]) {
                    if (p.slot == c) {
                        own = own != nullptr ? own : &p;
                        continue;
                    }
                    double px = p.absX;
                    if (p.slot != kNoSlot && spanOf_[p.slot] == s) {
                        px = orderOf[p.slot] < orderOf[c] ? sp.xlo : sp.xhi;
                    } else if (p.slot != kNoSlot) {
                        px = x_[p.slot] + p.offX;
                    }
                    lo = std::min(lo, px);
                    hi = std::max(hi, px);
                }
                if (own != nullptr && lo <= hi) {
                    bounds.insert(bounds.end(), {lo - own->offX, hi - own->offX});
                }
            }
            keepCentral(bounds, opts_.clusterBounds);
            return bounds;
        };
        const auto place = [&](Cluster &cl) {
            double target = x_[cells[cl.first]];
            if (!cl.bounds.empty()) {
                std::vector<double> scratch = cl.bounds;
                target = medianInterval(scratch).center();
            }
            cl.x = snap(target, s, cl.width);
        };

        clusters.clear();
        for (std::size_t k = 0; k < cells.size(); ++k) {
            Cluster cur{boundsOf(cells[k]), w_[cells[k]], 0.0, k};
            place(cur);
            while (!clusters.empty() && clusters.back().x + clusters.back().width > cur.x + kEps) {
                Cluster prev = std::move(clusters.back());
                clusters.pop_back();
                for (const double b : cur.bounds) {
                    prev.bounds.push_back(b - prev.width);
                }
                keepCentral(prev.bounds, opts_.clusterBounds);
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
                placed.push_back(Move{cells[k], x, y_[cells[k]]});
                x += w_[cells[k]];
            }
        }
        bool overlapping = false;
        for (std::size_t k = 1; k < cells.size(); ++k) {
            overlapping = overlapping || x_[cells[k - 1]] + w_[cells[k - 1]] > x_[cells[k]] + kEps;
        }
        const bool fits = placed.back().x + w_[cells.back()] <= sp.xhi + kEps;
        if (fits && (overlapping || wirelengthDelta(placed) < -kEps)) {
            for (const Move &m : placed) {
                moves += std::abs(x_[m.cell] - m.x) > kEps ? 1 : 0;
                x_[m.cell] = m.x;
            }
        }
        for (const std::size_t c : cells) {
            orderOf[c] = kNoSlot;
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
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(x_[i]);
        fy[mov_[i]] = static_cast<float>(y_[i]);
    }
    fastdp::FrameInfo info;
    info.note = note;
    info.step = step;
    info.hpwl = hpwl();
    info.hpwlInitial = info.hpwl;
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

void FastDetailedPlacer::Impl::selfCheck(DetailPlaceResult &res) const {
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
        if (std::fabs(q - std::round(q)) * sp.site > kEps * std::max(1.0, std::fabs(x_[i]))) {
            ++res.offSite;
        }
    }
    std::vector<std::size_t> all(mov_.size());
    std::iota(all.begin(), all.end(), 0u);
    std::ranges::sort(all, [&](std::size_t a, std::size_t b) {
        return y_[a] != y_[b] ? y_[a] < y_[b] : x_[a] < x_[b];
    });
    for (std::size_t k = 1; k < all.size(); ++k) {
        const std::size_t a = all[k - 1];
        const std::size_t b = all[k];
        if (y_[a] == y_[b] && x_[a] + w_[a] > x_[b] + kEps) {
            ++res.overlappingPairs;
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const long b = std::clamp<long>(static_cast<long>(std::floor((y_[i] - bandY0_) / bandY_)),
                                        0, static_cast<long>(fixedByBand_.size()) - 1);
        for (const std::size_t fi : fixedByBand_[static_cast<std::size_t>(b)]) {
            const FixedBox &f = fixed_[fi];
            if (x_[i] + w_[i] > f.x0 + kEps && x_[i] < f.x1 - kEps && y_[i] + h_[i] > f.y0 + kEps &&
                y_[i] < f.y1 - kEps) {
                ++res.overFixed;
                break;
            }
        }
    }
}

DetailPlaceResult FastDetailedPlacer::Impl::place(const FastdpOptions &params) {
    DetailPlaceResult res;
    ScopedTimer timer("detail-place");
    opts_ = params;
    mov_.clear();
    w_.clear();
    h_.clear();
    x_.clear();
    y_.clear();
    region_.clear();
    fixed_.clear();
    tallSkipped_ = 0;
    bandY_ = kInf;

    double rowHeight = kInf;
    for (const fastdp::RowInfo &ri : design.rows) {
        if (ri.height > 0.0) {
            rowHeight = std::min(rowHeight, ri.height);
        }
    }
    if (!std::isfinite(rowHeight)) {
        rowHeight = 1.0;
    }
    collectCells(rowHeight);
    if (mov_.empty()) {
        return res;
    }
    buildSpans();
    indexFixed();
    buildNetlist();
    res.hpwlBefore = hpwl();
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
        return before > 0.0 && (before - hpwl()) / before >= opts_.minImprovement;
    };
    const auto cluster = [&] {
        res.clusterMoves += timed(clusterSeconds, [&] {
            return clusterSegments();
        });
    };

    std::size_t step = 0;
    if (opts_.clustering) {
        cluster();
    }
    for (std::size_t iteration = 0; iteration < opts_.maxIterations; ++iteration) {
        const double before = hpwl();
        if (opts_.globalSwap) {
            res.globalSwaps += timed(globalSeconds, [&] {
                return globalSwap();
            });
        }
        if (opts_.verticalSwap) {
            res.verticalSwaps += timed(verticalSeconds, [&] {
                return verticalSwap();
            });
        }
        if (opts_.localReorder) {
            res.reorderMoves += timed(reorderSeconds, [&] {
                return localReorder();
            });
        }
        writeFrame("global swap, vertical swap, local re-ordering", ++step);
        if (!improvedEnough(before)) {
            break;
        }
    }
    if (opts_.clustering) {
        for (std::size_t iteration = 0; iteration < opts_.maxIterations; ++iteration) {
            const double before = hpwl();
            cluster();
            if (!improvedEnough(before)) {
                break;
            }
        }
        writeFrame("single-segment clustering", ++step);
    }
    ktlog.echo("  detail-place global      {:>8.3f}s", globalSeconds);
    ktlog.echo("  detail-place vertical    {:>8.3f}s", verticalSeconds);
    ktlog.echo("  detail-place reorder     {:>8.3f}s", reorderSeconds);
    ktlog.echo("  detail-place cluster     {:>8.3f}s", clusterSeconds);

    res.hpwlAfter = hpwl();
    selfCheck(res);
    res.seconds = timer.elapsedSeconds();
    ktlog.echo(
        "FastDP: {:.3f}s, HPWL {:.6e} -> {:.6e} ({:+.2f}%), swaps {} global / {} vertical, "
        "reorder {}, cluster {}",
        res.seconds, res.hpwlBefore, res.hpwlAfter,
        res.hpwlBefore > 0.0 ? 100.0 * (res.hpwlAfter - res.hpwlBefore) / res.hpwlBefore : 0.0,
        res.globalSwaps, res.verticalSwaps, res.reorderMoves, res.clusterMoves);
    if (tallSkipped_ > 0) {
        ktlog.echo("  detail-place left {} multi-row cell(s) where legalization put them",
                   tallSkipped_);
    }
    return res;
}

FastDetailedPlacer::FastDetailedPlacer(const fastdp::Design &design)
    : pImpl(std::make_unique<Impl>(design)) {}

FastDetailedPlacer::~FastDetailedPlacer() = default;

namespace {

void report(const DetailPlaceResult &r) {
    ktReportTable t("Detailed placement (FastDP)");
    t.setHeaders({"metric", "value"});
    t.addRow({"global swaps", std::format("{}", r.globalSwaps)});
    t.addRow({"vertical swaps", std::format("{}", r.verticalSwaps)});
    t.addRow({"reorder moves", std::format("{}", r.reorderMoves)});
    t.addRow({"cluster moves", std::format("{}", r.clusterMoves)});
    t.addRow({"HPWL before", std::format("{:.6}", r.hpwlBefore)});
    t.addRow({"HPWL after", std::format("{:.6}", r.hpwlAfter)});
    t.addRow({"HPWL change", std::format("{:.2}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                                       (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"time (s)", std::format("{:.6}", r.seconds)});
    t.addRow({"overlapping pairs", std::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", std::format("{}", r.offRow)});
    t.addRow({"cells off site", std::format("{}", r.offSite)});
    t.addRow({"cells over macro", std::format("{}", r.overFixed)});
    t.emit();
    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
}

}  // namespace

DetailPlaceResult FastDetailedPlacer::place(const FastdpOptions &options) {
    DetailPlaceResult result = pImpl->place(options);
    report(result);
    return result;
}

ktplace::fastdpSolution FastDetailedPlacer::solution() const {
    fastdpSolution out;
    const std::size_t n = pImpl->design.cells.size();
    out.xs.resize(n);
    out.ys.resize(n);
    for (std::size_t v = 0; v < n; ++v) {
        out.xs[v] = pImpl->design.cells[v].x;
        out.ys[v] = pImpl->design.cells[v].y;
    }
    for (std::size_t i = 0; i < pImpl->mov_.size(); ++i) {
        out.xs[pImpl->mov_[i]] = pImpl->x_[i];
        out.ys[pImpl->mov_[i]] = pImpl->y_[i];
    }
    return out;
}

}  // namespace ktplace
