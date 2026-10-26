#include "detailPlacer/fastdp/fastdp_placer.h"

#include "datamodel/kt_hpwl.h"
#include "detailPlacer/fastdp/fastdp_options.h"
#include "geometry/kt_geometry.h"
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

    [[nodiscard]] double getWidth() const noexcept {
        return std::floor(xhi / site + eps) * site - std::ceil(xlo / site - eps) * site;
    }
    [[nodiscard]] bool contains(double px, double pw) const noexcept {
        return px >= xlo - eps && px + pw <= xhi + eps;
    }
};

struct CellPin {
    std::size_t net = 0;
    Point offset;
};

struct OptimalRegion {
    Interval x;
    Interval y;
};

struct Move {
    std::size_t cell = noSlot;
    Point position;
};

struct NetBox {
    BBox box = BBox::getEmpty();
    std::uint32_t atXlo = 0;
    std::uint32_t atXhi = 0;
    std::uint32_t atYlo = 0;
    std::uint32_t atYhi = 0;
};

struct NetChange {
    BBox moved = BBox::getEmpty();
    std::uint32_t fromXlo = 0;
    std::uint32_t fromXhi = 0;
    std::uint32_t fromYlo = 0;
    std::uint32_t fromYhi = 0;
    bool stale = false;
    bool counted = false;
};

Interval getMedianInterval(std::vector<double> &values) {
    const std::size_t k = values.size() / 2;
    std::ranges::nth_element(values, values.begin() + static_cast<std::ptrdiff_t>(k));
    const double hi = values[k];
    const double lo =
        k == 0 ? hi
               : *std::max_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k));
    return {.lo = lo, .hi = hi};
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

double getSecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

struct Stats {
    std::size_t globalSwaps = 0;
    std::size_t verticalSwaps = 0;
    std::size_t reorderMoves = 0;
    std::size_t clusterMoves = 0;
    std::size_t rolledBack = 0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    double seconds = 0.0;
    double globalSeconds = 0.0;
    double verticalSeconds = 0.0;
    double reorderSeconds = 0.0;
    double clusterSeconds = 0.0;
};

void report(const Stats &r) {
    ktReportTable t("Detailed placement (FastDP)");
    t.setHeaders({"metric", "value"});
    t.addRow({"global swaps", std::format("{} ({:.3f}s)", r.globalSwaps, r.globalSeconds)});
    t.addRow({"vertical swaps", std::format("{} ({:.3f}s)", r.verticalSwaps, r.verticalSeconds)});
    t.addRow({"reorder moves", std::format("{} ({:.3f}s)", r.reorderMoves, r.reorderSeconds)});
    t.addRow({"cluster moves", std::format("{} ({:.3f}s)", r.clusterMoves, r.clusterSeconds)});
    t.addRow({"passes rolled back", std::format("{}", r.rolledBack)});
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
    t.emit();
    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
}

}  // namespace

class FastDetailedPlacer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    Stats place(const FastdpOptions &params);

    Design &design;
    const std::vector<std::size_t> &mov = design.cells.id;
    std::vector<double> &cellX = design.cells.x;
    std::vector<double> &cellY = design.cells.y;

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
        std::vector<std::uint32_t> netChange;
        std::vector<NetChange> changes;
        std::uint32_t stamp = 0;
    };

    struct Plan {
        bool active = false;
        double targetX = 0.0;
        std::vector<Candidate> found;
    };

    void buildSpans();
    void assignCells();
    void indexFixed();
    void buildNetlist();

    [[nodiscard]] double getHpwl() const;
    [[nodiscard]] double getWirelengthDelta(std::span<const Move> moves) const;
    [[nodiscard]] double getWirelengthDelta(std::span<const Move> moves, DeltaScratch &sc) const;
    [[nodiscard]] DeltaScratch getFreshScratch() const;
    [[nodiscard]] double getNetDelta(std::size_t n, std::span<const Move> moves,
                                     const std::vector<std::size_t> &moveOf) const;
    [[nodiscard]] NetBox measure(std::size_t n) const;
    void refreshBoxes() const;
    void moveCell(std::size_t c, const Point &to);
    [[nodiscard]] OptimalRegion getOptimalRegion(std::size_t c) const;

    [[nodiscard]] std::size_t getIndexIn(std::size_t s, std::size_t c) const;
    [[nodiscard]] double getGap(std::size_t s, std::ptrdiff_t g) const;
    [[nodiscard]] double snap(double x, std::size_t s, double w) const;
    [[nodiscard]] double getPenalty(double excess, double nearRoom, double wideRoom) const;
    [[nodiscard]] double getCellPenalty(std::size_t s, std::size_t k, double excess) const;
    [[nodiscard]] std::size_t getNearestRow(double y) const;

    [[nodiscard]] Candidate swapWith(std::size_t i, std::size_t j, DeltaScratch &sc) const;
    [[nodiscard]] Candidate moveInto(std::size_t i, std::size_t s, std::size_t g, double target,
                                     DeltaScratch &sc) const;
    [[nodiscard]] std::size_t getGapAt(std::size_t s, double x) const;
    [[nodiscard]] Plan getGlobalPlan(std::size_t i, DeltaScratch &sc) const;
    [[nodiscard]] Plan getVerticalPlan(std::size_t i, DeltaScratch &sc) const;
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
    [[nodiscard]] Window getWindowAt(std::size_t s, std::size_t k) const;
    [[nodiscard]] Window getWholeSpan(std::size_t s) const;
    void legalizeWindow(const Window &w);

    std::size_t globalSwap();
    std::size_t verticalSwap();
    std::size_t keepShorter(std::size_t (Impl::*sweep)(), bool &batchPays);
    void legalizeBatch();
    std::size_t localReorder();
    std::size_t clusterSegments();
    std::size_t clusterWindow(const Window &w);
    [[nodiscard]] std::vector<Move> clusterProposal(const Window &w,
                                                    std::vector<std::size_t> &order) const;
    std::size_t commitCluster(const Window &w, const std::vector<Move> &placed);
    [[nodiscard]] std::vector<Move> getBestOrder(std::size_t s, std::size_t k,
                                                 DeltaScratch &sc) const;

    void selfCheck(Stats &res) const;

    struct Snapshot {
        std::vector<double> x;
        std::vector<double> y;
        std::vector<std::size_t> spanOf;
        std::vector<Span> spans;
        std::vector<double> used;
    };
    [[nodiscard]] Snapshot getSnapshot() const;
    void restore(Snapshot saved);
    void writeFrame(const std::string &note, std::size_t step = 0) const;
    double hpwlInput = 0.0;

    FastdpOptions opts;
    const std::vector<double> &cellW = design.cells.width;
    const std::vector<double> &cellH = design.cells.height;
    const std::vector<int> &cellRegion = design.cells.region;
    std::vector<char> stranded;
    std::vector<std::size_t> spanOf;
    std::vector<Span> spans;
    std::vector<double> used;
    std::vector<std::vector<std::size_t>> spansInRow;
    std::vector<double> rowY;
    const Nets &netPins = design.nets;
    std::vector<std::vector<std::size_t>> cellNets;
    std::vector<std::vector<CellPin>> cellPins;
    mutable std::vector<NetBox> netBox;
    mutable std::vector<char> netStale;
    mutable std::vector<std::size_t> staleNets;
    const std::vector<BBox> &fixedBoxes = design.fixedBoxes;
    std::vector<std::vector<std::size_t>> fixedByBand;
    double bandY = inf;
    double bandY0 = 0.0;

    std::vector<std::size_t> orderOf;
    bool batched = false;
    std::vector<std::size_t> batchSpans;
    mutable DeltaScratch deltaScratch;
};

void FastDetailedPlacer::Impl::buildSpans() {
    const std::vector<RowInfo> &rows = design.rows;
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
        for (const BBox &b : design.blockages) {
            block(b.lowerLeft.x, b.upperRight.x, b.lowerLeft.y, b.upperRight.y);
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
                        spans.push_back(Span{.ylo = ri.coordinate,
                                             .yhi = ri.coordinate + ri.height,
                                             .xlo = lo,
                                             .xhi = hi,
                                             .site = ri.getPitch() > 0.0 ? ri.getPitch() : 1.0,
                                             .row = r,
                                             .region = sub.region,
                                             .cells = {}});
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
        const std::size_t r = getNearestRow(cellY[i]);
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
        for (long b = band(fixedBoxes[i].lowerLeft.y); b <= band(fixedBoxes[i].upperRight.y); ++b) {
            fixedByBand[static_cast<std::size_t>(b)].push_back(i);
        }
    }
}

void FastDetailedPlacer::Impl::buildNetlist() {
    cellNets.assign(mov.size(), {});
    for (std::size_t n = 0; n < netPins.size(); ++n) {
        for (const NetPin &p : netPins[n]) {
            if (p.slot != noSlot && (cellNets[p.slot].empty() || cellNets[p.slot].back() != n)) {
                cellNets[p.slot].push_back(n);
            }
        }
    }
    cellPins.assign(mov.size(), {});
    for (std::size_t n = 0; n < netPins.size(); ++n) {
        for (const NetPin &p : netPins[n]) {
            if (p.slot != noSlot) {
                cellPins[p.slot].push_back({.net = n, .offset = p.offset});
            }
        }
    }
    netBox.assign(netPins.size(), {});
    netStale.assign(netPins.size(), 1);
    staleNets.resize(netPins.size());
    std::iota(staleNets.begin(), staleNets.end(), std::size_t{0});
    refreshBoxes();
    deltaScratch = getFreshScratch();
    orderOf.assign(mov.size(), noSlot);
}

NetBox FastDetailedPlacer::Impl::measure(std::size_t n) const {
    NetBox out;
    const auto at = [this](const NetPin &p) {
        return p.slot == noSlot
                   ? p.fixedPosition
                   : Point{.x = cellX[p.slot] + p.offset.x, .y = cellY[p.slot] + p.offset.y};
    };
    for (const NetPin &p : netPins[n]) {
        const auto [x, y] = at(p);
        out.box.extend(x, y);
    }
    for (const NetPin &p : netPins[n]) {
        const auto [x, y] = at(p);
        out.atXlo += x == out.box.lowerLeft.x ? 1 : 0;
        out.atXhi += x == out.box.upperRight.x ? 1 : 0;
        out.atYlo += y == out.box.lowerLeft.y ? 1 : 0;
        out.atYhi += y == out.box.upperRight.y ? 1 : 0;
    }
    return out;
}

void FastDetailedPlacer::Impl::refreshBoxes() const {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, staleNets.size()),
                      [this](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t k = range.begin(); k < range.end(); ++k) {
                              const std::size_t n = staleNets[k];
                              if (netStale[n] != 0) {
                                  netBox[n] = measure(n);
                              }
                          }
                      });
    for (const std::size_t n : staleNets) {
        netStale[n] = 0;
    }
    staleNets.clear();
}

void FastDetailedPlacer::Impl::moveCell(std::size_t c, const Point &to) {
    const auto leave = [](double at, double edge, std::uint32_t &count) {
        if (at != edge) {
            return true;
        }
        if (count > 1) {
            --count;
            return true;
        }
        return false;
    };
    const auto reachLow = [](double at, double &edge, std::uint32_t &count) {
        if (at < edge) {
            edge = at;
            count = 1;
        } else if (at == edge) {
            ++count;
        }
    };
    const auto reachHigh = [](double at, double &edge, std::uint32_t &count) {
        if (at > edge) {
            edge = at;
            count = 1;
        } else if (at == edge) {
            ++count;
        }
    };
    for (const CellPin &pin : cellPins[c]) {
        const std::size_t n = pin.net;
        if (netStale[n] != 0) {
            continue;
        }
        NetBox &b = netBox[n];
        const double ox = cellX[c] + pin.offset.x;
        const double oy = cellY[c] + pin.offset.y;
        if (!leave(ox, b.box.lowerLeft.x, b.atXlo) || !leave(ox, b.box.upperRight.x, b.atXhi) ||
            !leave(oy, b.box.lowerLeft.y, b.atYlo) || !leave(oy, b.box.upperRight.y, b.atYhi)) {
            netStale[n] = 1;
            staleNets.push_back(n);
            continue;
        }
        const double nx = to.x + pin.offset.x;
        const double ny = to.y + pin.offset.y;
        reachLow(nx, b.box.lowerLeft.x, b.atXlo);
        reachHigh(nx, b.box.upperRight.x, b.atXhi);
        reachLow(ny, b.box.lowerLeft.y, b.atYlo);
        reachHigh(ny, b.box.upperRight.y, b.atYhi);
    }
    cellX[c] = to.x;
    cellY[c] = to.y;
}

double FastDetailedPlacer::Impl::getHpwl() const {
    return getNetHpwl(
        netPins.size(),
        [this](std::size_t n) {
            return netPins[n];
        },
        [this](const NetPin &p) {
            return p.slot == noSlot
                       ? p.fixedPosition
                       : Point{.x = cellX[p.slot] + p.offset.x, .y = cellY[p.slot] + p.offset.y};
        });
}

FastDetailedPlacer::Impl::DeltaScratch FastDetailedPlacer::Impl::getFreshScratch() const {
    return {.moveOf = std::vector<std::size_t>(mov.size(), noSlot),
            .netStamp = std::vector<std::uint32_t>(design.nets.size(), 0),
            .netChange = std::vector<std::uint32_t>(design.nets.size(), 0),
            .changes = {},
            .stamp = 0};
}

double FastDetailedPlacer::Impl::getWirelengthDelta(std::span<const Move> moves) const {
    return getWirelengthDelta(moves, deltaScratch);
}

double FastDetailedPlacer::Impl::getNetDelta(std::size_t n, std::span<const Move> moves,
                                             const std::vector<std::size_t> &moveOf) const {
    BBox before = BBox::getEmpty();
    BBox after = BBox::getEmpty();
    for (const NetPin &p : netPins[n]) {
        if (p.slot == noSlot) {
            before.extend(p.fixedPosition);
            after.extend(p.fixedPosition);
            continue;
        }
        before.extend(cellX[p.slot] + p.offset.x, cellY[p.slot] + p.offset.y);
        if (const std::size_t k = moveOf[p.slot]; k != noSlot) {
            after.extend(moves[k].position.x + p.offset.x, moves[k].position.y + p.offset.y);
        } else {
            after.extend(cellX[p.slot] + p.offset.x, cellY[p.slot] + p.offset.y);
        }
    }
    return after.getHalfPerimeter() - before.getHalfPerimeter();
}

double FastDetailedPlacer::Impl::getWirelengthDelta(std::span<const Move> moves,
                                                    DeltaScratch &sc) const {
    const bool serial = &sc == &deltaScratch;
    if (++sc.stamp == 0) {
        std::ranges::fill(sc.netStamp, 0u);
        sc.stamp = 1;
    }
    const std::uint32_t stamp = sc.stamp;
    sc.changes.clear();
    for (std::size_t k = 0; k < moves.size(); ++k) {
        sc.moveOf[moves[k].cell] = k;
    }
    for (const Move &m : moves) {
        for (const CellPin &pin : cellPins[m.cell]) {
            const std::size_t n = pin.net;
            if (sc.netStamp[n] != stamp) {
                sc.netStamp[n] = stamp;
                sc.netChange[n] = static_cast<std::uint32_t>(sc.changes.size());
                sc.changes.emplace_back();
                if (serial && netStale[n] != 0) {
                    netBox[n] = measure(n);
                    netStale[n] = 0;
                }
                sc.changes.back().stale = netStale[n] != 0;
            }
            NetChange &change = sc.changes[sc.netChange[n]];
            if (change.stale) {
                continue;
            }
            const NetBox &b = netBox[n];
            const double ox = cellX[m.cell] + pin.offset.x;
            const double oy = cellY[m.cell] + pin.offset.y;
            change.fromXlo += ox == b.box.lowerLeft.x ? 1 : 0;
            change.fromXhi += ox == b.box.upperRight.x ? 1 : 0;
            change.fromYlo += oy == b.box.lowerLeft.y ? 1 : 0;
            change.fromYhi += oy == b.box.upperRight.y ? 1 : 0;
            change.moved.extend(m.position.x + pin.offset.x, m.position.y + pin.offset.y);
        }
    }
    double delta = 0.0;
    for (const Move &m : moves) {
        for (const std::size_t n : cellNets[m.cell]) {
            NetChange &change = sc.changes[sc.netChange[n]];
            if (change.counted) {
                continue;
            }
            change.counted = true;
            const NetBox &b = netBox[n];
            if (change.stale ||
                (change.fromXlo == b.atXlo && change.moved.lowerLeft.x > b.box.lowerLeft.x) ||
                (change.fromXhi == b.atXhi && change.moved.upperRight.x < b.box.upperRight.x) ||
                (change.fromYlo == b.atYlo && change.moved.lowerLeft.y > b.box.lowerLeft.y) ||
                (change.fromYhi == b.atYhi && change.moved.upperRight.y < b.box.upperRight.y)) {
                delta += getNetDelta(n, moves, sc.moveOf);
                continue;
            }
            BBox after = b.box;
            after.extend(change.moved.lowerLeft);
            after.extend(change.moved.upperRight);
            delta += after.getHalfPerimeter() - b.box.getHalfPerimeter();
        }
    }
    for (const Move &m : moves) {
        sc.moveOf[m.cell] = noSlot;
    }
    return delta;
}

OptimalRegion FastDetailedPlacer::Impl::getOptimalRegion(std::size_t c) const {
    std::vector<double> xs;
    std::vector<double> ys;
    for (const std::size_t n : cellNets[c]) {
        BBox others = BBox::getEmpty();
        const NetPin *own = nullptr;
        for (const NetPin &p : netPins[n]) {
            if (p.slot == c) {
                own = own != nullptr ? own : &p;
            } else if (p.slot == noSlot) {
                others.extend(p.fixedPosition);
            } else {
                others.extend(cellX[p.slot] + p.offset.x, cellY[p.slot] + p.offset.y);
            }
        }
        if (own == nullptr || others.lowerLeft.x > others.upperRight.x) {
            continue;
        }
        xs.insert(xs.end(),
                  {others.lowerLeft.x - own->offset.x, others.upperRight.x - own->offset.x});
        ys.insert(ys.end(),
                  {others.lowerLeft.y - own->offset.y, others.upperRight.y - own->offset.y});
    }
    if (xs.empty()) {
        return {.x = {.lo = cellX[c], .hi = cellX[c]}, .y = {.lo = cellY[c], .hi = cellY[c]}};
    }
    return {.x = getMedianInterval(xs), .y = getMedianInterval(ys)};
}

std::size_t FastDetailedPlacer::Impl::getIndexIn(std::size_t s, std::size_t c) const {
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

double FastDetailedPlacer::Impl::getGap(std::size_t s, std::ptrdiff_t g) const {
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

double FastDetailedPlacer::Impl::getPenalty(double excess, double nearRoom, double wideRoom) const {
    return std::max(0.0, excess - nearRoom) * opts.penaltyWeight1 +
           std::max(0.0, excess - wideRoom) * opts.penaltyWeight2;
}

double FastDetailedPlacer::Impl::getCellPenalty(std::size_t s, std::size_t k, double excess) const {
    if (excess <= 0.0) {
        return 0.0;
    }
    const auto g = static_cast<std::ptrdiff_t>(k);
    const double nearRoom = getGap(s, g) + getGap(s, g + 1);
    return getPenalty(excess, nearRoom, nearRoom + getGap(s, g - 1) + getGap(s, g + 2));
}

std::size_t FastDetailedPlacer::Impl::getNearestRow(double y) const {
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
    if (si != sj && (used[sj] - cellW[j] + cellW[i] > spans[sj].getWidth() + eps ||
                     used[si] - cellW[i] + cellW[j] > spans[si].getWidth() + eps)) {
        return {};
    }
    const double xi = snap(cellX[j] + 0.5 * (cellW[j] - cellW[i]), sj, cellW[i]);
    const double xj = snap(cellX[i] + 0.5 * (cellW[i] - cellW[j]), si, cellW[j]);
    const std::array<Move, 2> moves{Move{.cell = i, .position = {.x = xi, .y = spans[sj].ylo}},
                                    Move{.cell = j, .position = {.x = xj, .y = spans[si].ylo}}};
    const double overlap = cellW[i] > cellW[j]
                               ? getCellPenalty(sj, getIndexIn(sj, j), cellW[i] - cellW[j])
                               : getCellPenalty(si, getIndexIn(si, i), cellW[j] - cellW[i]);
    return {.benefit = -getWirelengthDelta(moves, sc) - overlap,
            .partner = j,
            .span = sj,
            .x = xi,
            .partnerX = xj};
}

FastDetailedPlacer::Impl::Candidate FastDetailedPlacer::Impl::moveInto(std::size_t i, std::size_t s,
                                                                       std::size_t g, double target,
                                                                       DeltaScratch &sc) const {
    if (s != spanOf[i] && used[s] + cellW[i] > spans[s].getWidth() + eps) {
        return {};
    }
    const Span &sp = spans[s];
    const auto gi = static_cast<std::ptrdiff_t>(g);
    const double room = getGap(s, gi);
    const double left = g == 0 ? sp.xlo : cellX[sp.cells[g - 1]] + cellW[sp.cells[g - 1]];
    const double x = room >= cellW[i]
                         ? snap(std::clamp(target, left, left + room - cellW[i]), s, cellW[i])
                         : snap(left + 0.5 * (room - cellW[i]), s, cellW[i]);
    const std::array<Move, 1> moves{Move{.cell = i, .position = {.x = x, .y = sp.ylo}}};
    const double overlap =
        room >= cellW[i] ? 0.0
                         : getPenalty(cellW[i], room, room + getGap(s, gi - 1) + getGap(s, gi + 1));
    return {.benefit = -getWirelengthDelta(moves, sc) - overlap,
            .partner = noSlot,
            .span = s,
            .x = x,
            .partnerX = 0.0};
}

FastDetailedPlacer::Impl::Plan FastDetailedPlacer::Impl::getGlobalPlan(std::size_t i,
                                                                       DeltaScratch &sc) const {
    if (spanOf[i] == noSlot) {
        return {};
    }
    const OptimalRegion region = getOptimalRegion(i);
    if (region.x.contains(cellX[i], eps) && region.y.contains(cellY[i], eps)) {
        return {};
    }
    const double cx = region.x.getCenter();
    const double cy = region.y.getCenter();
    std::vector<std::size_t> rows;
    const auto lo = std::ranges::lower_bound(rowY, region.y.lo - eps);
    const auto hi = std::ranges::upper_bound(rowY, region.y.hi + eps);
    for (auto r = lo; r != hi; ++r) {
        rows.push_back(static_cast<std::size_t>(r - rowY.begin()));
    }
    if (rows.empty()) {
        rows.push_back(getNearestRow(cy));
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
                if (region.x.contains(cellX[cells[k]], eps)) {
                    items.push_back(
                        {.distance = std::abs(cellX[cells[k]] - cx), .index = k, .isGap = false});
                }
            }
            for (std::size_t g = first; g <= last; ++g) {
                const double left = g == 0 ? sp.xlo : cellX[cells[g - 1]] + cellW[cells[g - 1]];
                const double right = g == cells.size() ? sp.xhi : cellX[cells[g]];
                if (right > left + eps && right >= region.x.lo - eps && left <= region.x.hi + eps) {
                    items.push_back({.distance = std::abs(std::clamp(cx, left, right) - cx),
                                     .index = g,
                                     .isGap = true});
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

std::size_t FastDetailedPlacer::Impl::getGapAt(std::size_t s, double x) const {
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
                                                  : moveInto(i, c.span, getGapAt(c.span, c.x),
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
        return getFreshScratch();
    });
    const std::size_t batch = std::max<std::size_t>(opts.parallelBatch, 1);
    std::vector<Plan> plans;
    std::size_t moves = 0;
    for (std::size_t begin = 0; begin < mov.size(); begin += batch) {
        const std::size_t end = std::min(mov.size(), begin + batch);
        plans.assign(end - begin, Plan{});
        refreshBoxes();
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
    const std::size_t vacated = getIndexIn(home, i);
    const std::array<double, 2> usedBefore{used[home], used[c.span]};
    std::vector<Move> before{Move{.cell = i, .position = {.x = cellX[i], .y = cellY[i]}}};
    if (c.partner != noSlot) {
        before.push_back(
            Move{.cell = c.partner, .position = {.x = cellX[c.partner], .y = cellY[c.partner]}});
    }

    apply(i, c);
    std::vector<std::pair<std::size_t, std::size_t>> anchors{{c.span, getIndexIn(c.span, i)}};
    if (c.partner != noSlot) {
        anchors.emplace_back(home, getIndexIn(home, c.partner));
    } else if (!spans[home].cells.empty()) {
        anchors.emplace_back(home, std::min(vacated, spans[home].cells.size() - 1));
    }
    for (const auto &[span, index] : anchors) {
        const Window w = getWindowAt(span, index);
        const std::vector<std::size_t> &cells = spans[w.span].cells;
        for (std::size_t k = w.first; k < w.last; ++k) {
            if (std::ranges::find(before, cells[k], &Move::cell) == before.end()) {
                before.push_back(Move{.cell = cells[k],
                                      .position = {.x = cellX[cells[k]], .y = cellY[cells[k]]}});
            }
        }
        legalizeWindow(w);
        clusterWindow(w);
    }
    std::erase_if(before, [this](const Move &m) {
        return std::abs(cellX[m.cell] - m.position.x) <= eps &&
               std::abs(cellY[m.cell] - m.position.y) <= eps;
    });
    if (getWirelengthDelta(before) > eps) {
        return true;
    }

    detach(i);
    if (c.partner != noSlot) {
        detach(c.partner);
    }
    for (const Move &m : before) {
        moveCell(m.cell, m.position);
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
    moveCell(c, {.x = nx, .y = cellY[c]});
    attach(c, s);
}

void FastDetailedPlacer::Impl::detach(std::size_t c) {
    const std::size_t s = spanOf[c];
    std::vector<std::size_t> &cells = spans[s].cells;
    cells.erase(cells.begin() + static_cast<std::ptrdiff_t>(getIndexIn(s, c)));
    used[s] -= cellW[c];
    spanOf[c] = noSlot;
}

void FastDetailedPlacer::Impl::attach(std::size_t c, std::size_t s) {
    moveCell(c, {.x = cellX[c], .y = spans[s].ylo});
    std::vector<std::size_t> &cells = spans[s].cells;
    cells.insert(std::ranges::upper_bound(cells, cellX[c], {},
                                          [this](std::size_t a) {
                                              return cellX[a];
                                          }),
                 c);
    used[s] += cellW[c];
    spanOf[c] = s;
}

FastDetailedPlacer::Impl::Window FastDetailedPlacer::Impl::getWholeSpan(std::size_t s) const {
    return {.span = s,
            .first = 0,
            .last = spans[s].cells.size(),
            .lo = spans[s].xlo,
            .hi = spans[s].xhi};
}

FastDetailedPlacer::Impl::Window FastDetailedPlacer::Impl::getWindowAt(std::size_t s,
                                                                       std::size_t k) const {
    const Span &sp = spans[s];
    const std::vector<std::size_t> &cells = sp.cells;
    for (std::size_t reach = std::max<std::size_t>(opts.verifyReach, 1);; reach *= 2) {
        const std::size_t first = k > reach ? k - reach : 0;
        const std::size_t last = std::min(cells.size(), k + reach + 1);
        if (first == 0 && last == cells.size()) {
            return getWholeSpan(s);
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
            return {.span = s, .first = first, .last = last, .lo = lo, .hi = hi};
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
        Cluster cur{.weight = 1.0, .q = cellX[c], .width = cellW[c], .x = 0.0, .first = k};
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
            moveCell(cells[k], {.x = x, .y = cellY[cells[k]]});
            x += cellW[cells[k]];
        }
    }
}

std::size_t FastDetailedPlacer::Impl::keepShorter(std::size_t (Impl::*sweep)(), bool &batchPays) {
    if (!batchPays) {
        return (this->*sweep)();
    }
    const double before = getHpwl();
    Snapshot saved = getSnapshot();
    batched = true;
    const std::size_t moves = (this->*sweep)();
    batched = false;
    legalizeBatch();
    if (getHpwl() < before) {
        return moves;
    }
    batchPays = false;
    restore(std::move(saved));
    return (this->*sweep)();
}

void FastDetailedPlacer::Impl::legalizeBatch() {
    std::ranges::stable_sort(batchSpans);
    const auto [from, to] = std::ranges::unique(batchSpans);
    batchSpans.erase(from, to);
    for (const std::size_t s : batchSpans) {
        if (used[s] <= spans[s].getWidth() + eps) {
            legalizeWindow(getWholeSpan(s));
        }
    }
    batchSpans.clear();
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    return sweepCells(&Impl::getGlobalPlan);
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    return sweepCells(&Impl::getVerticalPlan);
}

FastDetailedPlacer::Impl::Plan FastDetailedPlacer::Impl::getVerticalPlan(std::size_t i,
                                                                         DeltaScratch &sc) const {
    const std::size_t si = spanOf[i];
    if (si == noSlot) {
        return {};
    }
    const OptimalRegion region = getOptimalRegion(i);
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

std::vector<Move> FastDetailedPlacer::Impl::getBestOrder(std::size_t s, std::size_t k,
                                                         DeltaScratch &sc) const {
    const std::size_t n = std::min<std::size_t>(opts.reorderWindow, 6);
    const Span &sp = spans[s];
    const std::span<const std::size_t> group(sp.cells.data() + k, n);
    const double left = cellX[group.front()];
    const double right = cellX[group.back()] + cellW[group.back()];
    double widths = 0.0;
    for (const std::size_t c : group) {
        widths += cellW[c];
    }
    const double spacing = (right - left - widths) / static_cast<double>(n - 1);
    if (spacing < -eps) {
        return {};
    }
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::vector<Move> trial(n);
    std::vector<Move> best;
    double bestDelta = -eps;
    do {
        double x = left;
        for (std::size_t t = 0; t < n; ++t) {
            const std::size_t c = group[order[t]];
            trial[t] =
                Move{.cell = c,
                     .position = {.x = left + std::floor((x - left) / sp.site + eps) * sp.site,
                                  .y = cellY[c]}};
            x += cellW[c] + spacing;
        }
        if (const double delta = getWirelengthDelta(trial, sc); delta < bestDelta) {
            bestDelta = delta;
            best = trial;
        }
    } while (std::ranges::next_permutation(order).found);
    return best;
}

std::size_t FastDetailedPlacer::Impl::localReorder() {
    const std::size_t n = std::min<std::size_t>(opts.reorderWindow, 6);
    if (n < 2) {
        return 0;
    }
    struct Window3 {
        std::size_t span;
        std::size_t first;
    };
    tbb::enumerable_thread_specific<DeltaScratch> local([this] {
        return getFreshScratch();
    });
    std::size_t moves = 0;
    std::vector<Window3> windows;
    std::vector<std::vector<Move>> best;
    for (std::size_t offset = 0; offset < n; ++offset) {
        windows.clear();
        for (std::size_t s = 0; s < spans.size(); ++s) {
            for (std::size_t k = offset; k + n <= spans[s].cells.size(); k += n) {
                windows.push_back({.span = s, .first = k});
            }
        }
        best.assign(windows.size(), {});
        refreshBoxes();
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, windows.size()),
                          [&](const tbb::blocked_range<std::size_t> &range) {
                              DeltaScratch &sc = local.local();
                              for (std::size_t w = range.begin(); w < range.end(); ++w) {
                                  best[w] = getBestOrder(windows[w].span, windows[w].first, sc);
                              }
                          });
        for (std::size_t w = 0; w < windows.size(); ++w) {
            if (best[w].empty() || getWirelengthDelta(best[w]) >= -eps) {
                continue;
            }
            std::vector<std::size_t> &cells = spans[windows[w].span].cells;
            for (std::size_t t = 0; t < n; ++t) {
                const Move &m = best[w][t];
                moves += std::abs(cellX[m.cell] - m.position.x) > eps ? 1 : 0;
                moveCell(m.cell, {.x = m.position.x, .y = cellY[m.cell]});
                cells[windows[w].first + t] = m.cell;
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::clusterSegments() {
    tbb::enumerable_thread_specific<std::vector<std::size_t>> local([this] {
        return std::vector<std::size_t>(mov.size(), noSlot);
    });
    std::vector<std::vector<Move>> proposals(spans.size());
    refreshBoxes();
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, spans.size()),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          std::vector<std::size_t> &order = local.local();
                          for (std::size_t s = range.begin(); s < range.end(); ++s) {
                              proposals[s] = clusterProposal(getWholeSpan(s), order);
                          }
                      });
    std::size_t moves = 0;
    for (std::size_t s = 0; s < spans.size(); ++s) {
        moves += commitCluster(getWholeSpan(s), proposals[s]);
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::clusterWindow(const Window &w) {
    return commitCluster(w, clusterProposal(w, orderOf));
}

std::vector<Move> FastDetailedPlacer::Impl::clusterProposal(const Window &w,
                                                            std::vector<std::size_t> &order) const {
    struct Cluster {
        std::vector<double> bounds;
        double width;
        double x;
        std::size_t first;
    };
    std::vector<Move> placed;
    std::vector<Cluster> clusters;
    const std::vector<std::size_t> &cells = spans[w.span].cells;
    const double site = spans[w.span].site;
    if (w.first == w.last) {
        return placed;
    }
    for (std::size_t k = w.first; k < w.last; ++k) {
        order[cells[k]] = k;
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
                double px = p.fixedPosition.x;
                if (p.slot != noSlot && order[p.slot] != noSlot) {
                    px = order[p.slot] < order[c] ? w.lo : w.hi;
                } else if (p.slot != noSlot) {
                    px = cellX[p.slot] + p.offset.x;
                }
                lo = std::min(lo, px);
                hi = std::max(hi, px);
            }
            if (own != nullptr && lo <= hi) {
                bounds.insert(bounds.end(), {lo - own->offset.x, hi - own->offset.x});
            }
        }
        keepCentral(bounds, opts.clusterBounds);
        return bounds;
    };
    const auto place = [&](Cluster &cl) {
        double target = cellX[cells[cl.first]];
        if (!cl.bounds.empty()) {
            std::vector<double> scratch = cl.bounds;
            target = getMedianInterval(scratch).getCenter();
        }
        cl.x = snapWithin(target, site, cl.width, w.lo, w.hi);
    };

    for (std::size_t k = w.first; k < w.last; ++k) {
        Cluster cur{.bounds = boundsOf(cells[k]), .width = cellW[cells[k]], .x = 0.0, .first = k};
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
            placed.push_back(Move{.cell = cells[k], .position = {.x = x, .y = cellY[cells[k]]}});
            x += cellW[cells[k]];
        }
    }
    for (std::size_t k = w.first; k < w.last; ++k) {
        order[cells[k]] = noSlot;
    }
    return placed;
}

std::size_t FastDetailedPlacer::Impl::commitCluster(const Window &w,
                                                    const std::vector<Move> &placed) {
    if (placed.empty()) {
        return 0;
    }
    const std::vector<std::size_t> &cells = spans[w.span].cells;
    std::size_t moves = 0;
    bool overlapping = false;
    for (std::size_t k = w.first + 1; k < w.last; ++k) {
        overlapping =
            overlapping || cellX[cells[k - 1]] + cellW[cells[k - 1]] > cellX[cells[k]] + eps;
    }
    const bool fits = placed.back().position.x + cellW[cells[w.last - 1]] <= w.hi + eps;
    if (fits && (overlapping || getWirelengthDelta(placed) < -eps)) {
        for (const Move &m : placed) {
            moves += std::abs(cellX[m.cell] - m.position.x) > eps ? 1 : 0;
            moveCell(m.cell, {.x = m.position.x, .y = cellY[m.cell]});
        }
    }
    return moves;
}

void FastDetailedPlacer::Impl::writeFrame(const std::string &note, std::size_t step) const {
    if (!design.onFrame) {
        return;
    }
    const std::size_t nv = design.vertexX.size();
    std::vector<float> fx(nv);
    std::vector<float> fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(design.vertexX[v]);
        fy[v] = static_cast<float>(design.vertexY[v]);
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        fx[mov[i]] = static_cast<float>(cellX[i]);
        fy[mov[i]] = static_cast<float>(cellY[i]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.hpwl = getHpwl();
    info.hpwlInitial = hpwlInput;
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

void FastDetailedPlacer::Impl::selfCheck(Stats &res) const {
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
            const BBox &f = fixedBoxes[fi];
            if (cellX[i] + cellW[i] > f.lowerLeft.x + eps && cellX[i] < f.upperRight.x - eps &&
                cellY[i] + cellH[i] > f.lowerLeft.y + eps && cellY[i] < f.upperRight.y - eps) {
                ++res.overFixed;
                break;
            }
        }
    }
}

FastDetailedPlacer::Impl::Snapshot FastDetailedPlacer::Impl::getSnapshot() const {
    return {.x = cellX, .y = cellY, .spanOf = spanOf, .spans = spans, .used = used};
}

void FastDetailedPlacer::Impl::restore(Snapshot saved) {
    cellX = std::move(saved.x);
    cellY = std::move(saved.y);
    std::ranges::fill(netStale, 1);
    staleNets.resize(netPins.size());
    std::iota(staleNets.begin(), staleNets.end(), std::size_t{0});
    spanOf = std::move(saved.spanOf);
    spans = std::move(saved.spans);
    used = std::move(saved.used);
}

Stats FastDetailedPlacer::Impl::place(const FastdpOptions &params) {
    Stats res;
    ScopedTimer timer("fastdp");
    opts = params;
    bandY = inf;
    if (mov.empty()) {
        return res;
    }
    buildSpans();
    indexFixed();
    buildNetlist();
    res.hpwlBefore = getHpwl();
    hpwlInput = res.hpwlBefore;
    writeFrame("Input - legal placement");

    double globalSeconds = 0.0;
    double verticalSeconds = 0.0;
    double reorderSeconds = 0.0;
    double clusterSeconds = 0.0;
    const auto timed = [](double &total, auto &&technique) {
        const auto start = std::chrono::steady_clock::now();
        const std::size_t moves = technique();
        total += getSecondsSince(start);
        return moves;
    };
    const auto improvedEnough = [&](double before) {
        return before > 0.0 && (before - getHpwl()) / before >= opts.minImprovement;
    };
    const auto cluster = [&] {
        res.clusterMoves += timed(clusterSeconds, [&] {
            return clusterSegments();
        });
    };
    const auto keptBetter = [&](Snapshot saved, double before, std::string_view what) {
        if (getHpwl() <= before) {
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
    bool batchGlobal = true;
    bool batchVertical = true;
    for (std::size_t iteration = 0; iteration < opts.maxIterations; ++iteration) {
        const double before = getHpwl();
        Snapshot saved = getSnapshot();
        if (opts.globalSwap) {
            res.globalSwaps += timed(globalSeconds, [&] {
                return keepShorter(&Impl::globalSwap, batchGlobal);
            });
            writeFrame(std::format("Global swap - pass {}", iteration + 1), ++step);
        }
        if (opts.verticalSwap) {
            res.verticalSwaps += timed(verticalSeconds, [&] {
                return keepShorter(&Impl::verticalSwap, batchVertical);
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
            const double before = getHpwl();
            Snapshot saved = getSnapshot();
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

    res.hpwlAfter = getHpwl();
    selfCheck(res);
    res.seconds = timer.getElapsedSeconds();
    if (design.tallCells > 0) {
        ktlog.echo("left {} multi-row cell(s) where legalization put them", design.tallCells);
    }
    return res;
}

FastDetailedPlacer::FastDetailedPlacer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

FastDetailedPlacer::~FastDetailedPlacer() = default;

void FastDetailedPlacer::place(const FastdpOptions &options) {
    report(pImpl->place(options));
}

}  // namespace ktplace::fastdp
