#include "globalPlacer/ntuplace1/ntuplace1_placer.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace ktplace::ntuplace1 {
namespace {

struct RatioRegion {
    BBox box;
    std::vector<std::uint32_t> cells;  // indices into the placer's movable array
    std::size_t depth = 0;
};

double getOverlap(const BBox &a, const BBox &b) {
    const BBox overlap = a.getIntersection(b);
    const double w = overlap.getWidth();
    const double h = overlap.getHeight();
    return (w > 0.0 && h > 0.0) ? w * h : 0.0;
}

constexpr int maxPasses = 8;
constexpr double balanceSlack = 0.02;

struct Stats {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;
    std::size_t cuts = 0;
    std::size_t ratioRetries = 0;
    std::size_t maxDepth = 0;
    std::size_t minLeafCells = 0;
    double hpwlFinal = 0.0;
    double meanImbalance = 0.0;
    std::size_t fenceRegions = 0;
    std::size_t fenceViolations = 0;
};

void report(const Stats &r) {
    ktReportTable t("NTUplace1 solver results");
    t.setHeaders({"metric", "value"});
    t.addRow({"movable cells", std::format("{}", r.numMovable)});
    t.addRow({"fixed blocks", std::format("{}", r.numFixed)});
    t.addRow({"hypergraph nets", std::format("{}", r.nets)});
    t.addRow({"cuts accepted", std::format("{}", r.cuts)});
    t.addRow({"look-ahead retries", std::format("{}", r.ratioRetries)});
    t.addRow({"retries per cut", std::format("{:.3}", r.meanImbalance)});
    t.addRow({"recursion depth reached", std::format("{}", r.maxDepth)});
    t.addRow({"smallest leaf", std::format("{}", r.minLeafCells)});
    t.addRow({"HPWL estimate (pre-legalization)", std::format("{:.6}", r.hpwlFinal)});
    if (r.fenceRegions > 0) {
        t.addRow({"fence regions", std::format("{}", r.fenceRegions)});
        t.addRow({"cells outside their fence", std::format("{}", r.fenceViolations)});
    }
    t.emit();
}

}  // namespace

class RatioPlacer::Impl {
public:
    explicit Impl(Design &target) : design(target) {}

    Stats place(const Ntuplace1Options &params);

private:
    void build();
    double getFreeAreaIn(const BBox &area) const;
    double getMovableArea(const std::vector<std::uint32_t> &) const;
    void divide(RatioRegion r);
    [[nodiscard]] int getForcedSide(int region, const RatioRegion &r0, const RatioRegion &r1) const;
    std::size_t applyFences();
    std::size_t getBipartition(const std::vector<std::uint32_t> &cells, bool vertical, double cut,
                               double target, double tol, const std::vector<std::int8_t> &forced,
                               std::vector<std::uint8_t> &side);
    void placeLeaf(const RatioRegion &r);
    void setRegionCentre(const RatioRegion &r);
    void writeFrame(std::size_t depth, const char *note) const;

    Design &design;
    const std::vector<std::size_t> &mov = design.cells.id;
    const std::vector<double> &cellArea = design.cells.area;
    const std::vector<int> &cellRegion = design.cells.region;
    const std::vector<HyperNet> &hyperNets = design.hyperNets;
    const std::vector<std::vector<std::uint32_t>> &cellNets = design.cellNets;
    const std::vector<BBox> &rowRects = design.rowRects;
    const double &maxRowH = design.maxRowH;
    const std::vector<BBox> &fixedRects = design.fixedRects;
    const BBox &rowBox = design.rowBox;
    std::vector<double> posX, posY;
    Ntuplace1Options opts;
    const constraintMgr *fences = nullptr;
    Stats result;

    std::uint32_t gen = 0;
    std::vector<std::uint32_t> localGen, localOf;         // per movable
    std::vector<std::uint32_t> netGen;                    // per net
    std::vector<std::uint8_t> anchor;                     // per net: bit s = a fixed pin on side s
    std::vector<std::array<std::uint32_t, 2>> sideCount;  // per net: pins on each side
    std::vector<std::uint32_t> regionNets;
};

void RatioPlacer::Impl::build() {
    result.numMovable = mov.size();
    result.numFixed = fixedRects.size();
    result.nets = hyperNets.size();

    localGen.assign(mov.size(), 0);
    localOf.assign(mov.size(), 0);
    netGen.assign(hyperNets.size(), 0);
    anchor.assign(hyperNets.size(), 0);
    sideCount.assign(hyperNets.size(), {0, 0});
    gen = 0;

    posX.assign(mov.size(), 0.5 * (rowBox.lowerLeft.x + rowBox.upperRight.x));
    posY.assign(mov.size(), 0.5 * (rowBox.lowerLeft.y + rowBox.upperRight.y));
}

double RatioPlacer::Impl::getFreeAreaIn(const BBox &area) const {
    if (rowRects.empty()) {
        return std::max(0.0, area.getArea());
    }
    double rows = 0.0;
    auto it = std::lower_bound(rowRects.begin(), rowRects.end(), area.lowerLeft.y - maxRowH,
                               [](const BBox &r, double y) {
                                   return r.lowerLeft.y < y;
                               });
    for (; it != rowRects.end() && it->lowerLeft.y < area.upperRight.y; ++it) {
        rows += getOverlap(*it, area);
    }
    const BBox clipped = area.getIntersection(rowBox);
    double fixed = 0.0;
    if (clipped.upperRight.x > clipped.lowerLeft.x && clipped.upperRight.y > clipped.lowerLeft.y) {
        for (const BBox &f : fixedRects) {
            fixed += getOverlap(f, clipped);
        }
    }
    return std::max(0.0, rows - fixed);
}

double RatioPlacer::Impl::getMovableArea(const std::vector<std::uint32_t> &cells) const {
    double a = 0.0;
    for (const std::uint32_t c : cells) {
        a += cellArea[c];
    }
    return a;
}

std::size_t RatioPlacer::Impl::getBipartition(const std::vector<std::uint32_t> &cells,
                                              bool vertical, double cut, double target, double tol,
                                              const std::vector<std::int8_t> &forced,
                                              std::vector<std::uint8_t> &side) {
    const std::size_t n = cells.size();
    side.assign(n, 0);
    ++gen;
    for (std::uint32_t i = 0; i < n; ++i) {
        localGen[cells[i]] = gen;
        localOf[cells[i]] = i;
    }

    regionNets.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
        for (const std::uint32_t e : cellNets[cells[i]]) {
            if (netGen[e] == gen) {
                continue;
            }
            netGen[e] = gen;
            regionNets.push_back(e);
            const HyperNet &net = hyperNets[e];
            std::uint8_t mask = 0;
            for (const std::uint32_t c : net.cells) {
                if (localGen[c] == gen) {
                    continue;
                }
                const double p = vertical ? posX[c] : posY[c];
                mask |= (p < cut) ? 1 : (p > cut) ? 2 : 0;
            }
            if (net.fixedPins > 0) {
                const Interval span = vertical ? net.fixedPinBox.getX() : net.fixedPinBox.getY();
                const double lo = span.lo;
                const double hi = span.hi;
                mask |= (hi < cut) ? 1 : (lo > cut) ? 2 : 3;
            }
            anchor[e] = mask;
        }
    }

    const bool anyForced = !forced.empty();
    double a0 = 0.0;
    if (anyForced) {
        for (std::uint32_t i = 0; i < n; ++i) {
            if (forced[i] == 0) {
                a0 += cellArea[cells[i]];
            }
        }
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        if (anyForced && forced[i] >= 0) {
            side[i] = static_cast<std::uint8_t>(forced[i]);
            continue;
        }
        if (a0 + 0.5 * cellArea[cells[i]] <= target) {
            a0 += cellArea[cells[i]];
            side[i] = 0;
        } else {
            side[i] = 1;
        }
    }

    const auto gainOf = [&](std::uint32_t i) {
        const std::uint8_t s = side[i], t = 1 - s;
        int g = 0;
        for (const std::uint32_t e : cellNets[cells[i]]) {
            if (anchor[e] == 3) {
                continue;
            }
            if (sideCount[e][s] == 1) {
                ++g;  // the last pin on its side: moving it un-cuts the net
            }
            if (sideCount[e][t] == 0) {
                --g;  // the net was whole on this side and moving it cuts it
            }
        }
        return g;
    };
    const auto dev = [&](double a) {
        return std::fabs(a - target);
    };

    std::vector<int> gain(n, 0);
    std::vector<char> locked(n, 0);
    std::vector<std::uint32_t> moves;
    moves.reserve(n);
    using Entry = std::pair<int, std::uint32_t>;

    for (int pass = 0; pass < maxPasses; ++pass) {
        for (const std::uint32_t e : regionNets) {
            sideCount[e] = {static_cast<std::uint32_t>((anchor[e] & 1) ? 1 : 0),
                            static_cast<std::uint32_t>((anchor[e] & 2) ? 1 : 0)};
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            for (const std::uint32_t e : cellNets[cells[i]]) {
                ++sideCount[e][side[i]];
            }
        }
        std::priority_queue<Entry> heap[2];
        for (std::uint32_t i = 0; i < n; ++i) {
            gain[i] = gainOf(i);
            locked[i] = (anyForced && forced[i] >= 0) ? 1 : 0;
            if (!locked[i]) {
                heap[side[i]].emplace(gain[i], i);
            }
        }

        moves.clear();
        int cum = 0, bestCum = 0;
        std::size_t bestLen = 0;
        double bestDev = dev(a0);
        double cur = a0;
        while (true) {
            int pick = -1;
            int pickGain = std::numeric_limits<int>::min();
            for (int s = 0; s < 2; ++s) {
                auto &h = heap[s];
                while (!h.empty() &&
                       (locked[h.top().second] || gain[h.top().second] != h.top().first ||
                        side[h.top().second] != s)) {
                    h.pop();
                }
                if (h.empty()) {
                    continue;
                }
                const std::uint32_t i = h.top().second;
                const double na = (s == 0) ? cur - cellArea[cells[i]] : cur + cellArea[cells[i]];
                if (dev(na) > tol && dev(na) >= dev(cur)) {
                    continue;
                }
                if (h.top().first > pickGain) {
                    pickGain = h.top().first;
                    pick = s;
                }
            }
            if (pick < 0) {
                break;
            }
            const std::uint32_t i = heap[pick].top().second;
            heap[pick].pop();
            const std::uint8_t s = side[i], t = 1 - s;
            locked[i] = 1;
            side[i] = t;
            cur += (s == 0) ? -cellArea[cells[i]] : cellArea[cells[i]];
            cum += pickGain;
            moves.push_back(i);

            for (const std::uint32_t e : cellNets[cells[i]]) {
                if (anchor[e] == 3) {
                    continue;
                }
                const bool critical = sideCount[e][t] <= 1 || sideCount[e][s] <= 2;
                --sideCount[e][s];
                ++sideCount[e][t];
                if (!critical) {
                    continue;
                }
                for (const std::uint32_t c : hyperNets[e].cells) {
                    if (localGen[c] != gen) {
                        continue;
                    }
                    const std::uint32_t j = localOf[c];
                    if (locked[j]) {
                        continue;
                    }
                    const int g = gainOf(j);
                    if (g != gain[j]) {
                        gain[j] = g;
                        heap[side[j]].emplace(g, j);
                    }
                }
            }
            if (dev(cur) <= tol && (cum > bestCum || (cum == bestCum && dev(cur) < bestDev))) {
                bestCum = cum;
                bestLen = moves.size();
                bestDev = dev(cur);
            }
        }
        for (std::size_t k = moves.size(); k > bestLen; --k) {
            const std::uint32_t i = moves[k - 1];
            cur += (side[i] == 0) ? -cellArea[cells[i]] : cellArea[cells[i]];
            side[i] = 1 - side[i];
        }
        a0 = cur;
        if (bestLen == 0) {
            break;  // no improving prefix: a further pass would find the same
        }
    }

    for (const std::uint32_t e : regionNets) {
        sideCount[e] = {static_cast<std::uint32_t>((anchor[e] & 1) ? 1 : 0),
                        static_cast<std::uint32_t>((anchor[e] & 2) ? 1 : 0)};
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        for (const std::uint32_t e : cellNets[cells[i]]) {
            ++sideCount[e][side[i]];
        }
    }
    std::size_t cutNets = 0;
    for (const std::uint32_t e : regionNets) {
        if (anchor[e] != 3 && sideCount[e][0] > 0 && sideCount[e][1] > 0) {
            ++cutNets;
        }
    }
    return cutNets;
}

void RatioPlacer::Impl::setRegionCentre(const RatioRegion &r) {
    const auto [cx, cy] = r.box.getCenter();
    for (const std::uint32_t c : r.cells) {
        posX[c] = cx;
        posY[c] = cy;
    }
}

void RatioPlacer::Impl::placeLeaf(const RatioRegion &r) {
    const std::size_t k = r.cells.size();
    if (k == 0) {
        return;
    }
    const double w = std::max(r.box.getWidth(), 1e-9), h = std::max(r.box.getHeight(), 1e-9);
    const std::size_t cols =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::sqrt(k * w / h))));
    const std::size_t rowsN = (k + cols - 1) / cols;
    for (std::size_t q = 0; q < k; ++q) {
        const std::size_t cx = q % cols, cy = q / cols;
        posX[r.cells[q]] =
            r.box.lowerLeft.x + w * (static_cast<double>(cx) + 0.5) / static_cast<double>(cols);
        posY[r.cells[q]] =
            r.box.lowerLeft.y + h * (static_cast<double>(cy) + 0.5) / static_cast<double>(rowsN);
    }
}

void RatioPlacer::Impl::writeFrame(std::size_t depth, const char *note) const {
    if (!design.onFrame) {
        return;
    }
    std::vector<float> xs(design.vertexX.size(), 0.0f);
    std::vector<float> ys(design.vertexY.size(), 0.0f);
    for (std::size_t v = 0; v < design.vertexX.size(); ++v) {
        xs[v] = static_cast<float>(design.vertexX[v]);
        ys[v] = static_cast<float>(design.vertexY[v]);
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        xs[mov[i]] = static_cast<float>(posX[i] - 0.5 * design.cells.width[i]);
        ys[mov[i]] = static_cast<float>(posY[i] - 0.5 * design.cells.height[i]);
    }
    design.onFrame(xs, ys, FrameInfo{.note = note, .step = depth, .numSteps = opts.maxLevels});
}

int RatioPlacer::Impl::getForcedSide(int region, const RatioRegion &r0,
                                     const RatioRegion &r1) const {
    const Region *reg = fences->getRegion(region);
    if (reg == nullptr) {
        return -1;
    }
    const auto overlapArea = [&](const RatioRegion &b) {
        double a = 0.0;
        for (const BBox &rc : reg->rects) {
            a += getOverlap(rc, b.box);
        }
        return a;
    };
    const double a0 = overlapArea(r0), a1 = overlapArea(r1);
    if (a0 > 0.0 && a1 <= 0.0) {
        return 0;
    }
    if (a1 > 0.0 && a0 <= 0.0) {
        return 1;
    }
    return -1;
}

std::size_t RatioPlacer::Impl::applyFences() {
    if (fences == nullptr || fences->getNumRegions() == 0) {
        return 0;
    }
    std::size_t outside = 0;
    for (std::size_t i = 0; i < mov.size(); ++i) {
        Point p{.x = design.cells.x[i], .y = design.cells.y[i]};
        if (cellRegion[i] != constraintMgr::noRegion) {
            fences->clampToRegion(cellRegion[i], p);
            outside += fences->contains(cellRegion[i], p) ? 0u : 1u;
        } else {
            (void)fences->pushOutOfRegions(p, rowBox);
        }
        design.cells.x[i] = p.x;
        design.cells.y[i] = p.y;
    }
    return outside;
}

void RatioPlacer::Impl::divide(RatioRegion r) {
    result.maxDepth = std::max(result.maxDepth, r.depth);
    if (r.cells.size() <= opts.targetLeafCells || r.depth >= opts.maxLevels) {
        if (!r.cells.empty()) {
            result.minLeafCells = (result.minLeafCells == 0)
                                      ? r.cells.size()
                                      : std::min(result.minLeafCells, r.cells.size());
        }
        placeLeaf(r);
        return;
    }

    const double w = r.box.getWidth();
    const double h = r.box.getHeight();
    const bool vertical = (w >= h);
    const double cut = vertical ? r.box.lowerLeft.x + 0.5 * w : r.box.lowerLeft.y + 0.5 * h;

    RatioRegion r0, r1;
    r0.box = r1.box = r.box;
    r0.depth = r1.depth = r.depth + 1;
    (vertical ? r0.box.upperRight.x : r0.box.upperRight.y) = cut;
    (vertical ? r1.box.lowerLeft.x : r1.box.lowerLeft.y) = cut;

    const double free0 = getFreeAreaIn(r0.box);
    const double free1 = getFreeAreaIn(r1.box);
    if (free0 <= 0.0 || free1 <= 0.0) {
        RatioRegion &keep = (free0 > 0.0) ? r0 : r1;
        if (free0 <= 0.0 && free1 <= 0.0) {
            placeLeaf(r);
            return;
        }
        keep.cells = std::move(r.cells);
        setRegionCentre(keep);
        divide(std::move(keep));
        return;
    }

    const double total = getMovableArea(r.cells);
    const double target = total * free0 / (free0 + free1);
    double maxCell = 0.0;
    for (const std::uint32_t c : r.cells) {
        maxCell = std::max(maxCell, cellArea[c]);
    }
    double tol = std::max(balanceSlack * total, 2.0 * maxCell);

    std::vector<std::int8_t> forced;
    if (fences != nullptr && fences->getNumRegions() > 0) {
        bool any = false;
        forced.assign(r.cells.size(), -1);
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            const int reg = cellRegion[r.cells[i]];
            if (reg != constraintMgr::noRegion) {
                forced[i] = static_cast<std::int8_t>(getForcedSide(reg, r0, r1));
                any = any || forced[i] >= 0;
            }
        }
        if (!any) {
            forced.clear();
        }
    }

    std::vector<std::uint8_t> side;
    std::size_t cutNets = 0;
    for (std::size_t attempt = 0;; ++attempt) {
        cutNets = getBipartition(r.cells, vertical, cut, target, tol, forced, side);
        r0.cells.clear();
        r1.cells.clear();
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            (side[i] == 0 ? r0.cells : r1.cells).push_back(r.cells[i]);
        }
        const double a0 = getMovableArea(r0.cells), a1 = total - a0;
        const bool fits = (a0 <= free0 && a1 <= free1) || total > free0 + free1;
        if (fits || attempt >= opts.maxRatioRetries || tol <= 2.0 * maxCell) {
            break;
        }
        ++result.ratioRetries;
        tol = std::max(0.5 * tol, 2.0 * maxCell);
    }

    ++result.cuts;
    ktlog.trace("  ratio cut depth {}: {} cells -> {} / {} (target {:.1f}%), {} nets cut", r.depth,
                r.cells.size(), r0.cells.size(), r1.cells.size(), 100.0 * free0 / (free0 + free1),
                cutNets);
    setRegionCentre(r0);
    setRegionCentre(r1);
    writeFrame(r.depth, "ratio bipartition");
    divide(std::move(r0));
    divide(std::move(r1));
}

Stats RatioPlacer::Impl::place(const Ntuplace1Options &params) {
    opts = params;
    fences = &design.fences;
    build();
    if (mov.empty()) {
        return result;
    }
    ktlog.trace("  ratio build: {} movable cells, {} fixed blocks, {} hypernets", result.numMovable,
                result.numFixed, result.nets);

    RatioRegion root;
    root.box = rowBox;
    root.cells.resize(mov.size());
    std::iota(root.cells.begin(), root.cells.end(), 0u);
    divide(std::move(root));

    for (std::size_t i = 0; i < mov.size(); ++i) {
        const double w = design.cells.width[i];
        const double h = design.cells.height[i];
        design.cells.x[i] = std::clamp(posX[i] - 0.5 * w, rowBox.lowerLeft.x,
                                       std::max(rowBox.lowerLeft.x, rowBox.upperRight.x - w));
        design.cells.y[i] = std::clamp(posY[i] - 0.5 * h, rowBox.lowerLeft.y,
                                       std::max(rowBox.lowerLeft.y, rowBox.upperRight.y - h));
    }
    result.fenceRegions = fences != nullptr ? fences->getNumRegions() : 0;
    result.fenceViolations = applyFences();

    double total = 0.0;
    for (const HyperNet &n : hyperNets) {
        BBox box = BBox::getEmpty();
        for (const std::uint32_t c : n.cells) {
            box.extend(posX[c], posY[c]);
        }
        box.extend(n.fixedPinBox);
        total += box.getHalfPerimeter();
    }
    result.hpwlFinal = total;
    result.meanImbalance = result.cuts > 0 ? static_cast<double>(result.ratioRetries) /
                                                 static_cast<double>(result.cuts)
                                           : 0.0;
    ktlog.trace("  ratio placement: {} cuts, {} retries, {} levels, HPWL {:.6e}", result.cuts,
                result.ratioRetries, result.maxDepth, result.hpwlFinal);
    return result;
}

RatioPlacer::RatioPlacer(Design &design) : pImpl(new Impl(design)) {}
RatioPlacer::~RatioPlacer() = default;
RatioPlacer::RatioPlacer(RatioPlacer &&) noexcept = default;
RatioPlacer &RatioPlacer::operator=(RatioPlacer &&) noexcept = default;

void RatioPlacer::place(const Ntuplace1Options &options) {
    report(pImpl->place(options));
}

}  // namespace ktplace::ntuplace1
