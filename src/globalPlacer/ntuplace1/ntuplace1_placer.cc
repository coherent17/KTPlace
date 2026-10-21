#include "globalPlacer/ntuplace1/ntuplace1_placer.h"

#include "util/kt_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace ktplace::ntuplace1 {
namespace {

struct HyperNet {
    std::vector<std::uint32_t> cells;
    double fx0 = 0.0, fx1 = 0.0, fy0 = 0.0, fy1 = 0.0;  // fixed pin points, min/max
    std::size_t fixedPins = 0;
};

struct RatioRegion {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    std::vector<std::uint32_t> cells;  // indices into the placer's movable array
    std::size_t depth = 0;
};

struct AreaRect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

double overlap(const AreaRect &a, double x0, double y0, double x1, double y1) {
    const double w = std::min(a.x1, x1) - std::max(a.x0, x0);
    const double h = std::min(a.y1, y1) - std::max(a.y0, y0);
    return (w > 0.0 && h > 0.0) ? w * h : 0.0;
}

constexpr std::uint32_t noIndex = 0xFFFFFFFFu;
constexpr int maxPasses = 8;
constexpr double balanceSlack = 0.02;
constexpr double lookAheadStep = 0.05;

double imbalanceWindow(double total, double cap0, double cap1) {
    const auto maxImbalance = [total](double cap) {
        return cap >= total ? std::numeric_limits<double>::infinity() : cap / (total - cap) - 1.0;
    };
    const double f = std::min(maxImbalance(cap0), maxImbalance(cap1));
    if (std::isinf(f)) {
        return 0.5 * total;
    }
    return std::max(0.0, total * f / (2.0 * (2.0 + f)));
}

std::pair<std::uint8_t, double> cutWeight(double tmin, double tmax, double p1, double p2) {
    if (tmin > tmax) {
        return {0, p2 - p1};
    }
    const double cost0 = std::max(tmax, p1) - std::min(tmin, p1);
    const double cost1 = std::max(tmax, p2) - std::min(tmin, p2);
    if (cost0 < cost1) {
        return {1, cost1 - cost0};
    }
    if (cost1 < cost0) {
        return {2, cost0 - cost1};
    }
    return {0, std::max(tmax, p2) - std::min(tmin, p1) - cost0};
}

}  // namespace

class RatioPlacer::Impl {
public:
    explicit Impl(Design &target) : design(target) {}

    Ntuplace1Result place(const Ntuplace1Options &params);

private:
    void build();
    double freeAreaIn(double x0, double y0, double x1, double y1) const;
    double movableArea(const std::vector<std::uint32_t> &) const;
    void divide(RatioRegion r);
    [[nodiscard]] double ratioCut(const RatioRegion &r, bool vertical, double total) const;
    void trimToFree(RatioRegion &r) const;
    [[nodiscard]] bool firstFit(const std::vector<std::uint32_t> &cells,
                                const RatioRegion &r) const;
    [[nodiscard]] int forcedSide(int region, const RatioRegion &r0, const RatioRegion &r1) const;
    std::size_t applyFences();
    std::size_t bipartition(const std::vector<std::uint32_t> &cells, bool vertical, double p1,
                            double p2, double target, double tol,
                            const std::vector<std::int8_t> &forced,
                            std::vector<std::uint8_t> &side);
    void setRegionCentre(const RatioRegion &r);
    void writeFrame(std::size_t depth, const char *note) const;

    Design &design;
    std::vector<std::uint32_t> mov;  // movable, non-terminal vertex ids
    std::vector<double> cellArea;
    std::vector<double> cellW;
    std::vector<double> cellH;  // area of each movable
    std::vector<int> cellRegion;
    std::vector<HyperNet> hyperNets;
    std::vector<std::vector<std::uint32_t>> cellNets;
    std::vector<AreaRect> rowRects;  // one per subrow, sorted by y
    double maxRowH = 0.0;
    std::vector<AreaRect> fixedRects;  // fixed blocks, terminals included
    std::array<double, 4> rowBox{};
    std::vector<double> posX, posY;
    Ntuplace1Options opts;
    const constraintMgr *fences = nullptr;
    Ntuplace1Result result;

    std::uint32_t gen = 0;
    std::vector<std::uint32_t> localGen, localOf;  // per movable
    std::vector<std::uint32_t> netGen;             // per net
    std::vector<std::uint8_t> anchor;              // per net: bit s = a fixed pin on side s
    std::vector<std::array<std::uint32_t, 2>> sideCount;
    std::vector<double> netWeight;  // per net: pins on each side
    std::vector<std::uint32_t> regionNets;
};

void RatioPlacer::Impl::build() {
    const std::size_t nv = design.cells.size();
    for (std::size_t v = 0; v < nv; ++v) {
        const Cell &vert = design.cells[v];
        if (vert.isFixed || vert.isTerminal) {
            if (vert.width > 0.0 && vert.height > 0.0) {
                fixedRects.push_back({vert.x, vert.y, vert.x + vert.width, vert.y + vert.height});
            }
            continue;
        }
        mov.push_back(static_cast<std::uint32_t>(v));
        cellArea.push_back(vert.width * vert.height);
        cellW.push_back(vert.width);
        cellH.push_back(vert.height);
        cellRegion.push_back(vert.region);
    }
    result.numMovable = mov.size();
    result.numFixed = fixedRects.size();

    std::vector<std::uint32_t> indexOf(nv, noIndex);
    for (std::size_t i = 0; i < mov.size(); ++i) {
        indexOf[mov[i]] = static_cast<std::uint32_t>(i);
    }

    hyperNets.clear();
    cellNets.assign(mov.size(), {});
    std::vector<std::size_t> lastNet(mov.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t v = 0; v < design.nets.size(); ++v) {
        HyperNet n;
        for (const std::size_t pinId : design.nets[v].pins) {
            const Pin &e = design.pins[pinId];
            const Cell &pin = design.cells[e.cellId];
            if (indexOf[e.cellId] != noIndex) {
                const std::uint32_t c = indexOf[e.cellId];
                if (lastNet[c] != v) {
                    lastNet[c] = v;
                    n.cells.push_back(c);
                }
                continue;
            }
            const double px = pin.x + e.offsetX;
            const double py = pin.y + e.offsetY;
            if (n.fixedPins == 0) {
                n.fx0 = n.fx1 = px;
                n.fy0 = n.fy1 = py;
            } else {
                n.fx0 = std::min(n.fx0, px);
                n.fx1 = std::max(n.fx1, px);
                n.fy0 = std::min(n.fy0, py);
                n.fy1 = std::max(n.fy1, py);
            }
            ++n.fixedPins;
        }
        if (n.cells.empty() || n.cells.size() + n.fixedPins < 2) {
            continue;
        }
        const auto id = static_cast<std::uint32_t>(hyperNets.size());
        for (const std::uint32_t c : n.cells) {
            cellNets[c].push_back(id);
        }
        hyperNets.push_back(std::move(n));
    }
    result.nets = hyperNets.size();

    localGen.assign(mov.size(), 0);
    localOf.assign(mov.size(), 0);
    netGen.assign(hyperNets.size(), 0);
    anchor.assign(hyperNets.size(), 0);
    sideCount.assign(hyperNets.size(), {0, 0});
    netWeight.assign(hyperNets.size(), 0.0);
    gen = 0;

    rowBox = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
              -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    for (const RowInfo &r : design.rows) {
        for (const Subrow &sr : r.subrows) {
            const AreaRect rr{sr.xlo, r.coordinate, sr.xhi, r.coordinate + r.height};
            if (rr.x1 <= rr.x0) {
                continue;
            }
            rowRects.push_back(rr);
            rowBox[0] = std::min(rowBox[0], rr.x0);
            rowBox[1] = std::min(rowBox[1], rr.y0);
            rowBox[2] = std::max(rowBox[2], rr.x1);
            rowBox[3] = std::max(rowBox[3], rr.y1);
        }
    }
    if (rowRects.empty()) {
        rowBox = design.die;
    }
    std::stable_sort(rowRects.begin(), rowRects.end(), [](const AreaRect &a, const AreaRect &b) {
        return a.y0 < b.y0;
    });
    maxRowH = 0.0;
    for (const AreaRect &r : rowRects) {
        maxRowH = std::max(maxRowH, r.y1 - r.y0);
    }
    posX.assign(mov.size(), 0.5 * (rowBox[0] + rowBox[2]));
    posY.assign(mov.size(), 0.5 * (rowBox[1] + rowBox[3]));
}

double RatioPlacer::Impl::freeAreaIn(double x0, double y0, double x1, double y1) const {
    if (rowRects.empty()) {
        return std::max(0.0, (x1 - x0) * (y1 - y0));
    }
    double rows = 0.0;
    auto it = std::lower_bound(rowRects.begin(), rowRects.end(), y0 - maxRowH,
                               [](const AreaRect &r, double y) {
                                   return r.y0 < y;
                               });
    for (; it != rowRects.end() && it->y0 < y1; ++it) {
        rows += overlap(*it, x0, y0, x1, y1);
    }
    const double cx0 = std::max(x0, rowBox[0]), cy0 = std::max(y0, rowBox[1]);
    const double cx1 = std::min(x1, rowBox[2]), cy1 = std::min(y1, rowBox[3]);
    double fixed = 0.0;
    if (cx1 > cx0 && cy1 > cy0) {
        for (const AreaRect &f : fixedRects) {
            fixed += overlap(f, cx0, cy0, cx1, cy1);
        }
    }
    return std::max(0.0, rows - fixed);
}

double RatioPlacer::Impl::movableArea(const std::vector<std::uint32_t> &cells) const {
    double a = 0.0;
    for (const std::uint32_t c : cells) {
        a += cellArea[c];
    }
    return a;
}

std::size_t RatioPlacer::Impl::bipartition(const std::vector<std::uint32_t> &cells, bool vertical,
                                           double p1, double p2, double target, double tol,
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
            double tmin = std::numeric_limits<double>::max();
            double tmax = std::numeric_limits<double>::lowest();
            for (const std::uint32_t c : net.cells) {
                if (localGen[c] == gen) {
                    continue;
                }
                const double p = vertical ? posX[c] : posY[c];
                tmin = std::min(tmin, p);
                tmax = std::max(tmax, p);
            }
            if (net.fixedPins > 0) {
                tmin = std::min(tmin, vertical ? net.fx0 : net.fy0);
                tmax = std::max(tmax, vertical ? net.fx1 : net.fy1);
            }
            const auto [mask, weight] = cutWeight(tmin, tmax, p1, p2);
            anchor[e] = weight > 0.0 ? mask : 3;
            netWeight[e] = weight;
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
        double g = 0.0;
        for (const std::uint32_t e : cellNets[cells[i]]) {
            if (anchor[e] == 3) {
                continue;
            }
            if (sideCount[e][s] == 1) {
                g += netWeight[e];
            }
            if (sideCount[e][t] == 0) {
                g -= netWeight[e];
            }
        }
        return g;
    };
    const auto dev = [&](double a) {
        return std::fabs(a - target);
    };

    std::vector<double> gain(n, 0.0);
    std::vector<char> locked(n, 0);
    std::vector<std::uint32_t> moves;
    moves.reserve(n);
    using Entry = std::pair<double, std::uint32_t>;

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
        double cum = 0.0;
        double bestCum = 0.0;
        std::size_t bestLen = 0;
        double bestDev = dev(a0);
        double cur = a0;
        while (true) {
            int pick = -1;
            double pickGain = std::numeric_limits<double>::lowest();
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
                    const double g = gainOf(j);
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
    const double cx = 0.5 * (r.x0 + r.x1), cy = 0.5 * (r.y0 + r.y1);
    for (const std::uint32_t c : r.cells) {
        posX[c] = cx;
        posY[c] = cy;
    }
}

void RatioPlacer::Impl::writeFrame(std::size_t depth, const char *note) const {
    if (!design.onFrame) {
        return;
    }
    std::vector<float> xs(design.cells.size(), 0.0f);
    std::vector<float> ys(design.cells.size(), 0.0f);
    for (std::size_t v = 0; v < design.cells.size(); ++v) {
        xs[v] = static_cast<float>(design.cells[v].x);
        ys[v] = static_cast<float>(design.cells[v].y);
    }
    for (std::size_t i = 0; i < mov.size(); ++i) {
        const Cell &v = design.cells[mov[i]];
        xs[mov[i]] = static_cast<float>(posX[i] - 0.5 * v.width);
        ys[mov[i]] = static_cast<float>(posY[i] - 0.5 * v.height);
    }
    design.onFrame(xs, ys, FrameInfo{note, depth, opts.maxLevels});
}

int RatioPlacer::Impl::forcedSide(int region, const RatioRegion &r0, const RatioRegion &r1) const {
    const Region *reg = fences->region(region);
    if (reg == nullptr) {
        return -1;
    }
    const auto overlapArea = [&](const RatioRegion &b) {
        double a = 0.0;
        for (const Rect &rc : reg->rects) {
            const double w = std::min(rc.hi.x, b.x1) - std::max(rc.lo.x, b.x0);
            const double h = std::min(rc.hi.y, b.y1) - std::max(rc.lo.y, b.y0);
            if (w > 0.0 && h > 0.0) {
                a += w * h;
            }
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
    if (fences == nullptr || fences->numRegions() == 0) {
        return 0;
    }
    std::size_t outside = 0;
    for (std::size_t i = 0; i < mov.size(); ++i) {
        double &x = design.cells[mov[i]].x;
        double &y = design.cells[mov[i]].y;
        if (cellRegion[i] != constraintMgr::noRegion) {
            fences->clampToRegion(cellRegion[i], x, y);
            outside += fences->contains(cellRegion[i], x, y) ? 0u : 1u;
        } else {
            (void)fences->pushOutOfRegions(x, y, rowBox[0], rowBox[1], rowBox[2], rowBox[3]);
        }
    }
    return outside;
}

double RatioPlacer::Impl::ratioCut(const RatioRegion &r, bool vertical, double total) const {
    const double lo = vertical ? r.x0 : r.y0;
    const double hi = vertical ? r.x1 : r.y1;
    const auto below = [&](double c) {
        return vertical ? freeAreaIn(r.x0, r.y0, c, r.y1) : freeAreaIn(r.x0, r.y0, r.x1, c);
    };
    const auto above = [&](double c) {
        return vertical ? freeAreaIn(c, r.y0, r.x1, r.y1) : freeAreaIn(r.x0, c, r.x1, r.y1);
    };
    const double middle = lo + 0.5 * (hi - lo);
    const double half = 0.5 * total;
    if (below(middle) >= half && above(middle) >= half) {
        return middle;
    }
    const auto bisect = [&](double a, double b, auto &&enough) {
        for (int step = 0; step < 40; ++step) {
            const double m = 0.5 * (a + b);
            (enough(m) ? b : a) = m;
        }
        return b;
    };
    if (below(middle) < half) {
        const double c = bisect(middle, hi, [&](double m) {
            return below(m) >= half;
        });
        return above(c) >= half ? c : middle;
    }
    const double c = -bisect(-middle, -lo, [&](double m) {
        return above(-m) >= half;
    });
    return below(c) >= half ? c : middle;
}

void RatioPlacer::Impl::trimToFree(RatioRegion &r) const {
    const double room = freeAreaIn(r.x0, r.y0, r.x1, r.y1);
    if (room <= 0.0) {
        return;
    }
    const double some = 1e-9 * room;
    const auto edge = [](double from, double to, auto &&hasRoom) {
        for (int step = 0; step < 40; ++step) {
            const double m = 0.5 * (from + to);
            (hasRoom(m) ? to : from) = m;
        }
        return from;
    };
    const RatioRegion box = r;
    r.x0 = edge(box.x0, box.x1, [&](double m) {
        return freeAreaIn(box.x0, box.y0, m, box.y1) > some;
    });
    r.x1 = edge(box.x1, box.x0, [&](double m) {
        return freeAreaIn(m, box.y0, box.x1, box.y1) > some;
    });
    r.y0 = edge(box.y0, box.y1, [&](double m) {
        return freeAreaIn(box.x0, box.y0, box.x1, m) > some;
    });
    r.y1 = edge(box.y1, box.y0, [&](double m) {
        return freeAreaIn(box.x0, m, box.x1, box.y1) > some;
    });
}

bool RatioPlacer::Impl::firstFit(const std::vector<std::uint32_t> &cells,
                                 const RatioRegion &r) const {
    std::vector<double> room;
    for (const AreaRect &row : rowRects) {
        const double mid = 0.5 * (row.y0 + row.y1);
        if (mid < r.y0 || mid >= r.y1) {
            continue;
        }
        const double a = std::max(row.x0, r.x0);
        const double b = std::min(row.x1, r.x1);
        if (b <= a) {
            continue;
        }
        std::vector<std::pair<double, double>> blocked;
        for (const AreaRect &f : fixedRects) {
            if (f.y1 > row.y0 && f.y0 < row.y1 && f.x1 > a && f.x0 < b) {
                blocked.emplace_back(std::max(f.x0, a), std::min(f.x1, b));
            }
        }
        std::ranges::stable_sort(blocked);
        double cursor = a;
        for (const auto &[x0, x1] : blocked) {
            if (x0 > cursor) {
                room.push_back(x0 - cursor);
            }
            cursor = std::max(cursor, x1);
        }
        if (b > cursor) {
            room.push_back(b - cursor);
        }
    }
    if (room.empty()) {
        return cells.empty();
    }

    std::size_t leaves = 1;
    while (leaves < room.size()) {
        leaves *= 2;
    }
    std::vector<double> best(2 * leaves, -1.0);
    for (std::size_t i = 0; i < room.size(); ++i) {
        best[leaves + i] = room[i];
    }
    for (std::size_t i = leaves - 1; i > 0; --i) {
        best[i] = std::max(best[2 * i], best[2 * i + 1]);
    }
    const auto take = [&](double w) {
        if (best[1] < w) {
            return false;
        }
        std::size_t i = 1;
        while (i < leaves) {
            i = best[2 * i] >= w ? 2 * i : 2 * i + 1;
        }
        best[i] -= w;
        for (i /= 2; i > 0; i /= 2) {
            best[i] = std::max(best[2 * i], best[2 * i + 1]);
        }
        return true;
    };

    std::vector<std::uint32_t> order(cells);
    std::ranges::stable_sort(order, std::greater<>{}, [&](std::uint32_t c) {
        return cellW[c];
    });
    const double rowH = maxRowH > 0.0 ? maxRowH : 1.0;
    for (const std::uint32_t c : order) {
        const auto pieces =
            std::max<std::size_t>(1, static_cast<std::size_t>(std::lround(cellH[c] / rowH)));
        for (std::size_t k = 0; k < pieces; ++k) {
            if (!take(cellW[c])) {
                return false;
            }
        }
    }
    return true;
}

void RatioPlacer::Impl::divide(RatioRegion r) {
    result.maxDepth = std::max(result.maxDepth, r.depth);
    if (r.cells.size() <= opts.targetLeafCells || r.depth >= opts.maxLevels) {
        if (!r.cells.empty()) {
            result.minLeafCells = (result.minLeafCells == 0)
                                      ? r.cells.size()
                                      : std::min(result.minLeafCells, r.cells.size());
        }
        setRegionCentre(r);
        return;
    }

    const bool vertical = (r.x1 - r.x0) >= (r.y1 - r.y0);
    const double total = movableArea(r.cells);
    const double lo = vertical ? r.x0 : r.y0;
    const double hi = vertical ? r.x1 : r.y1;
    const double step = lookAheadStep * (hi - lo);
    double cut = ratioCut(r, vertical, total);
    double maxCell = 0.0;
    for (const std::uint32_t c : r.cells) {
        maxCell = std::max(maxCell, cellArea[c]);
    }

    RatioRegion r0;
    RatioRegion r1;
    double free0 = 0.0;
    double free1 = 0.0;
    std::vector<std::uint8_t> side;
    std::size_t cutNets = 0;
    for (std::size_t attempt = 0;; ++attempt) {
        r0 = RatioRegion{r.x0, r.y0, r.x1, r.y1, {}, r.depth + 1};
        r1 = r0;
        (vertical ? r0.x1 : r0.y1) = cut;
        (vertical ? r1.x0 : r1.y0) = cut;
        trimToFree(r0);
        trimToFree(r1);
        free0 = freeAreaIn(r0.x0, r0.y0, r0.x1, r0.y1);
        free1 = freeAreaIn(r1.x0, r1.y0, r1.x1, r1.y1);
        if (attempt == 0 && (free0 <= 0.0 || free1 <= 0.0)) {
            if (free0 <= 0.0 && free1 <= 0.0) {
                setRegionCentre(r);
                return;
            }
            RatioRegion &keep = (free0 > 0.0) ? r0 : r1;
            keep.cells = std::move(r.cells);
            setRegionCentre(keep);
            divide(std::move(keep));
            return;
        }

        const bool overfull = total > free0 + free1;
        const double target = overfull ? total * free0 / (free0 + free1) : 0.5 * total;
        const double tol = std::max(
            overfull ? balanceSlack * total : imbalanceWindow(total, free0, free1), 2.0 * maxCell);

        std::vector<std::int8_t> forced;
        if (fences != nullptr && fences->numRegions() > 0) {
            bool any = false;
            forced.assign(r.cells.size(), -1);
            for (std::size_t i = 0; i < r.cells.size(); ++i) {
                const int reg = cellRegion[r.cells[i]];
                if (reg != constraintMgr::noRegion) {
                    forced[i] = static_cast<std::int8_t>(forcedSide(reg, r0, r1));
                    any = any || forced[i] >= 0;
                }
            }
            if (!any) {
                forced.clear();
            }
        }

        const double p1 = vertical ? 0.5 * (r0.x0 + r0.x1) : 0.5 * (r0.y0 + r0.y1);
        const double p2 = vertical ? 0.5 * (r1.x0 + r1.x1) : 0.5 * (r1.y0 + r1.y1);
        cutNets = bipartition(r.cells, vertical, p1, p2, target, tol, forced, side);
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            (side[i] == 0 ? r0.cells : r1.cells).push_back(r.cells[i]);
        }
        const bool legal = overfull || (firstFit(r0.cells, r0) && firstFit(r1.cells, r1));
        if (legal || attempt >= opts.maxRatioRetries) {
            break;
        }
        ++result.ratioRetries;
        const double use0 = movableArea(r0.cells) / free0;
        const double use1 = movableArea(r1.cells) / free1;
        const double moved = use0 > use1 ? cut + step : cut - step;
        if (moved <= lo + step || moved >= hi - step) {
            break;
        }
        cut = moved;
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

Ntuplace1Result RatioPlacer::Impl::place(const Ntuplace1Options &params) {
    opts = params;
    fences = &design.fences;
    build();
    if (mov.empty()) {
        return result;
    }
    ktlog.trace("  ratio build: {} movable cells, {} fixed blocks, {} hypernets", result.numMovable,
                result.numFixed, result.nets);

    RatioRegion root;
    root.x0 = rowBox[0];
    root.y0 = rowBox[1];
    root.x1 = rowBox[2];
    root.y1 = rowBox[3];
    root.cells.resize(mov.size());
    std::iota(root.cells.begin(), root.cells.end(), 0u);
    divide(std::move(root));

    for (std::size_t i = 0; i < mov.size(); ++i) {
        Cell &v = design.cells[mov[i]];
        v.x = std::clamp(posX[i] - 0.5 * v.width, rowBox[0],
                         std::max(rowBox[0], rowBox[2] - v.width));
        v.y = std::clamp(posY[i] - 0.5 * v.height, rowBox[1],
                         std::max(rowBox[1], rowBox[3] - v.height));
    }
    result.fenceRegions = fences != nullptr ? fences->numRegions() : 0;
    result.fenceViolations = applyFences();

    double total = 0.0;
    for (const HyperNet &n : hyperNets) {
        double ax = std::numeric_limits<double>::max(), bx = -ax, ay = ax, by = -ax;
        for (const std::uint32_t c : n.cells) {
            ax = std::min(ax, posX[c]);
            bx = std::max(bx, posX[c]);
            ay = std::min(ay, posY[c]);
            by = std::max(by, posY[c]);
        }
        if (n.fixedPins > 0) {
            ax = std::min(ax, n.fx0);
            bx = std::max(bx, n.fx1);
            ay = std::min(ay, n.fy0);
            by = std::max(by, n.fy1);
        }
        total += (bx - ax) + (by - ay);
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

Ntuplace1Result RatioPlacer::place(const Ntuplace1Options &options) {
    return pImpl->place(options);
}

}  // namespace ktplace::ntuplace1
