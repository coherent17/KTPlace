#include "globalPlacer/simpl/simpl_lal.h"

#include "globalPlacer/simpl/simpl_parallel.h"

#include <array>
#include <cstdint>
#include <limits>
#include <numeric>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_invoke.h>

namespace ktplace::simpl {

struct Block {
    BBox box;
    std::size_t level = 1;
    bool vertical = true;
    std::vector<std::uint32_t> cells;
    double regionRoom = 0.0;
};

struct Stripe {
    double lo = 0.0;
    double hi = 0.0;
    double capacity = 0.0;
    double used = 0.0;
    std::vector<std::uint32_t> cells;
};

double LookAheadLegalizer::getCentre(const std::vector<double> &pos, std::uint32_t i,
                                     bool vertical) const {
    return pos[i] + 0.5 * (vertical ? width[i] : height[i]);
}

double LookAheadLegalizer::getDensity(const std::vector<double> &occ, const BinRect &r) const {
    const double cells = grid.sum(occ, r);
    const double room = grid.sum(grid.avail, r);
    if (room > 0.0) {
        return cells / room;
    }
    return cells > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
}

void LookAheadLegalizer::legalize(std::vector<double> &x, std::vector<double> &y) const {
    std::vector<std::vector<std::uint32_t>> members(grid.size());
    std::vector<std::uint32_t> binOf(getNumMovable());
    std::vector<double> occ(grid.size(), 0.0);
    for (std::uint32_t i = 0; i < getNumMovable(); ++i) {
        const std::size_t k = grid.getBinOf(getCentre(x, i, true), getCentre(y, i, false));
        binOf[i] = static_cast<std::uint32_t>(k);
        members[k].push_back(i);
        occ[k] += area[i];
    }

    struct Turn {
        BinRect bins;
        BinRect reach;
        std::uint32_t id = 0;
    };
    const auto reachOf = [&](const BinRect &r) {
        return BinRect{r.x0 > 0 ? r.x0 - 1 : 0, std::min(r.x1 + 1, grid.nx - 1),
                       r.y0 > 0 ? r.y0 - 1 : 0, std::min(r.y1 + 1, grid.ny - 1)};
    };
    const auto touches = [](const BinRect &a, const BinRect &b) {
        return a.x0 <= b.x1 && b.x0 <= a.x1 && a.y0 <= b.y1 && b.y0 <= a.y1;
    };

    std::vector<std::uint32_t> taken(getNumMovable(), 0);
    const auto legalizeRegion = [&](const Turn &t) {
        Block top{grid.getBox(t.bins), 1, true, {}, grid.sum(grid.avail, t.bins)};
        for (std::size_t iy = t.bins.y0; iy <= t.bins.y1; ++iy) {
            for (std::size_t ix = t.bins.x0; ix <= t.bins.x1; ++ix) {
                const std::size_t k = grid.at(ix, iy);
                for (const std::uint32_t i : members[k]) {
                    if (binOf[i] == k && taken[i] != t.id) {
                        taken[i] = t.id;
                        top.cells.push_back(i);
                    }
                }
            }
        }
        const std::vector<std::uint32_t> cells = top.cells;
        legalizeBlock(top, x, y);
        for (const std::uint32_t i : cells) {
            const std::size_t k = grid.getBinOf(getCentre(x, i, true), getCentre(y, i, false));
            if (k != binOf[i]) {
                occ[binOf[i]] -= area[i];
                occ[k] += area[i];
                binOf[i] = static_cast<std::uint32_t>(k);
                members[k].push_back(i);
            }
        }
    };

    std::vector<Turn> batch;
    const auto runBatch = [&] {
        tbb::parallel_for(std::size_t{0}, batch.size(), [&](std::size_t b) {
            legalizeRegion(batch[b]);
        });
        batch.clear();
    };
    std::uint32_t turn = 0;
    for (const BinRect &cluster : getOverfilledClusters(occ)) {
        BinRect bins = getGrown(cluster, occ);
        const auto clashes = [&] {
            return std::ranges::any_of(batch, [&](const Turn &t) {
                return touches(t.reach, reachOf(bins));
            });
        };
        if (clashes()) {
            runBatch();
            bins = getGrown(cluster, occ);
        }
        batch.push_back({bins, reachOf(bins), ++turn});
    }
    runBatch();
    clampToDie(x, y);
}

void LookAheadLegalizer::legalizeBlock(Block &block, std::vector<double> &x,
                                       std::vector<double> &y) const {
    auto halves = split(block, x, y);
    if (!halves) {
        return;
    }
    tbb::parallel_invoke(
        [&] {
            legalizeBlock(halves->first, x, y);
        },
        [&] {
            legalizeBlock(halves->second, x, y);
        });
}

std::vector<BinRect> LookAheadLegalizer::getOverfilledClusters(
    const std::vector<double> &occ) const {
    std::vector<char> over(grid.size(), 0);
    for (std::size_t k = 0; k < grid.size(); ++k) {
        over[k] = occ[k] > densityLimit * grid.avail[k] ? 1 : 0;
    }

    std::vector<BinRect> regions;
    std::vector<std::size_t> frontier;
    for (std::size_t seed = 0; seed < grid.size(); ++seed) {
        if (!over[seed]) {
            continue;
        }
        over[seed] = 0;
        frontier.assign(1, seed);
        BinRect r{seed % grid.nx, seed % grid.nx, seed / grid.nx, seed / grid.nx};
        while (!frontier.empty()) {
            const std::size_t k = frontier.back();
            frontier.pop_back();
            const std::size_t ix = k % grid.nx;
            const std::size_t iy = k / grid.nx;
            r = r.getMerged({ix, ix, iy, iy});
            const std::array<std::pair<long, long>, 4> steps{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
            for (const auto &[sx, sy] : steps) {
                const long nx = static_cast<long>(ix) + sx;
                const long ny = static_cast<long>(iy) + sy;
                if (nx < 0 || ny < 0 || nx >= static_cast<long>(grid.nx) ||
                    ny >= static_cast<long>(grid.ny)) {
                    continue;
                }
                const std::size_t kk =
                    grid.at(static_cast<std::size_t>(nx), static_cast<std::size_t>(ny));
                if (over[kk]) {
                    over[kk] = 0;
                    frontier.push_back(kk);
                }
            }
        }
        regions.push_back(r);
    }
    return regions;
}

BinRect LookAheadLegalizer::getGrown(BinRect r, const std::vector<double> &occ) const {
    while (getDensity(occ, r) > densityLimit) {
        const std::array<BinRect, 4> wider{
            BinRect{r.x0 > 0 ? r.x0 - 1 : r.x0, r.x1, r.y0, r.y1},
            BinRect{r.x0, r.x1 + 1 < grid.nx ? r.x1 + 1 : r.x1, r.y0, r.y1},
            BinRect{r.x0, r.x1, r.y0 > 0 ? r.y0 - 1 : r.y0, r.y1},
            BinRect{r.x0, r.x1, r.y0, r.y1 + 1 < grid.ny ? r.y1 + 1 : r.y1}};
        const BinRect *best = nullptr;
        double bestDensity = std::numeric_limits<double>::infinity();
        for (const BinRect &w : wider) {
            if (w.getBins() == r.getBins()) {
                continue;
            }
            const double d = getDensity(occ, w);
            if (best == nullptr || d < bestDensity) {
                best = &w;
                bestDensity = d;
            }
        }
        if (best == nullptr) {
            break;
        }
        r = *best;
    }
    return r;
}

std::optional<std::pair<Block, Block>> LookAheadLegalizer::split(Block &region,
                                                                 std::vector<double> &x,
                                                                 std::vector<double> &y) const {
    if (region.level >= opts.maxLevel || region.cells.size() <= opts.minCellsToSplit) {
        return std::nullopt;
    }
    const bool vertical = region.vertical;
    std::vector<double> &pos = vertical ? x : y;

    std::vector<std::uint32_t> &cells = region.cells;
    sortCells(cells, [&](std::uint32_t i) {
        return getCentre(pos, i, vertical);
    });
    const double cellArea =
        std::accumulate(cells.begin(), cells.end(), 0.0, [&](double s, std::uint32_t i) {
            return s + area[i];
        });
    std::size_t m = 0;
    double lowArea = 0.0;
    while (m + 1 < cells.size() && (m == 0 || lowArea < 0.5 * cellArea)) {
        lowArea += area[cells[m++]];
    }
    std::vector<std::uint32_t> low(cells.begin(), cells.begin() + static_cast<long>(m));
    std::vector<std::uint32_t> high(cells.begin() + static_cast<long>(m), cells.end());

    const BBox &box = region.box;
    const double lo = vertical ? box.xlo : box.ylo;
    const double hi = vertical ? box.xhi : box.yhi;
    const AxisProfile room = freeSpace.getProfile(box, vertical);
    const double share = cellArea > 0.0 ? lowArea / cellArea : 0.5;
    const double cutline = room.getTotal() > 0.0 ? room.getCoordinateAt(share * room.getTotal())
                                                 : lo + share * (hi - lo);

    BBox lowBox = box;
    BBox highBox = box;
    (vertical ? lowBox.xhi : lowBox.yhi) = cutline;
    (vertical ? highBox.xlo : highBox.ylo) = cutline;

    nonlinearScale(low, lowBox, vertical, cutline, true, pos, region.regionRoom);
    nonlinearScale(high, highBox, vertical, cutline, false, pos, region.regionRoom);

    return std::pair{
        Block{lowBox, region.level + 1, !vertical, std::move(low), region.regionRoom},
        Block{highBox, region.level + 1, !vertical, std::move(high), region.regionRoom}};
}

std::vector<Stripe> LookAheadLegalizer::getStripes(const BBox &box, bool vertical, double cellArea,
                                                   double regionRoom) const {
    const double lo = vertical ? box.xlo : box.ylo;
    const double hi = vertical ? box.xhi : box.yhi;
    const AxisProfile room = freeSpace.getProfile(box, vertical);

    std::vector<double> cuts{lo, hi};
    for (const BBox &o : obstacles) {
        if (o.xhi <= box.xlo || o.xlo >= box.xhi || o.yhi <= box.ylo || o.ylo >= box.yhi) {
            continue;
        }
        for (const double edge : {vertical ? o.xlo : o.ylo, vertical ? o.xhi : o.yhi}) {
            if (edge > lo && edge < hi) {
                cuts.push_back(edge);
            }
        }
    }
    std::stable_sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    const double limit =
        opts.stripeAreaFraction * (regionRoom > 0.0 ? regionRoom : room.getTotal());
    const double fill =
        room.getTotal() > 0.0 ? std::min(densityLimit, cellArea / room.getTotal()) : densityLimit;
    for (std::size_t s = 0; s + 1 < cuts.size();) {
        const double a = cuts[s];
        const double b = cuts[s + 1];
        const double here = room.getBetween(a, b);
        const double mid = room.getCoordinateAt(room.getBefore(a) + 0.5 * here);
        if (here > limit && mid > a && mid < b) {
            cuts.insert(cuts.begin() + static_cast<long>(s) + 1, mid);
        } else {
            ++s;
        }
    }

    std::vector<Stripe> out;
    for (std::size_t s = 0; s + 1 < cuts.size(); ++s) {
        out.push_back(
            {cuts[s], cuts[s + 1], fill * room.getBetween(cuts[s], cuts[s + 1]), 0.0, {}});
    }
    return out;
}

void LookAheadLegalizer::nonlinearScale(const std::vector<std::uint32_t> &cells, const BBox &box,
                                        bool vertical, double cutline, bool belowCut,
                                        std::vector<double> &pos, double regionRoom) const {
    if (cells.empty()) {
        return;
    }
    const double cellArea =
        std::accumulate(cells.begin(), cells.end(), 0.0, [&](double sum, std::uint32_t i) {
            return sum + area[i];
        });
    std::vector<Stripe> strips = getStripes(box, vertical, cellArea, regionRoom);
    const auto fromCut = [&](double c) {
        return belowCut ? cutline - c : c - cutline;
    };
    std::stable_sort(strips.begin(), strips.end(), [&](const Stripe &a, const Stripe &b) {
        return fromCut(0.5 * (a.lo + a.hi)) > fromCut(0.5 * (b.lo + b.hi));
    });

    std::vector<std::uint32_t> order = cells;
    sortCells(order, [&](std::uint32_t i) {
        return -fromCut(getCentre(pos, i, vertical));
    });
    std::size_t s = 0;
    for (const std::uint32_t i : order) {
        while (s + 1 < strips.size() && strips[s].used >= strips[s].capacity) {
            ++s;
        }
        strips[s].cells.push_back(i);
        strips[s].used += area[i];
    }

    for (const Stripe &strip : strips) {
        if (strip.cells.empty()) {
            continue;
        }
        double lo = std::numeric_limits<double>::max();
        double hi = -lo;
        for (const std::uint32_t i : strip.cells) {
            lo = std::min(lo, getCentre(pos, i, vertical));
            hi = std::max(hi, getCentre(pos, i, vertical));
        }
        for (const std::uint32_t i : strip.cells) {
            const double t = hi > lo ? (getCentre(pos, i, vertical) - lo) / (hi - lo) : 0.5;
            const double size = vertical ? width[i] : height[i];
            const double c = strip.lo + t * (strip.hi - strip.lo);
            pos[i] = std::clamp(c - 0.5 * size, strip.lo, std::max(strip.lo, strip.hi - size));
        }
    }
}

void LookAheadLegalizer::clampToDie(std::vector<double> &x, std::vector<double> &y) const {
    for (std::size_t i = 0; i < getNumMovable(); ++i) {
        x[i] = std::clamp(x[i], die.xlo, std::max(die.xlo, die.xhi - width[i]));
        y[i] = std::clamp(y[i], die.ylo, std::max(die.ylo, die.yhi - height[i]));
    }
}

}  // namespace ktplace::simpl
