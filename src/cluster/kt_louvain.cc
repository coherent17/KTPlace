#include "cluster/kt_louvain.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <numeric>
#include <utility>
#include <vector>

namespace ktplace::cluster {

namespace {

constexpr double gainEpsilon = 1e-12;

}  // namespace

Level buildLevel(std::vector<double> area, std::vector<int> region, std::vector<double> selfLoop,
                 std::vector<Edge> edges) {
    std::ranges::stable_sort(edges, [](const Edge &a, const Edge &b) {
        return a.u != b.u ? a.u < b.u : a.v < b.v;
    });
    std::vector<Edge> merged;
    for (const Edge &e : edges) {
        if (!merged.empty() && merged.back().u == e.u && merged.back().v == e.v) {
            merged.back().w += e.w;
        } else {
            merged.push_back(e);
        }
    }
    Level level;
    const std::size_t n = area.size();
    level.area = std::move(area);
    level.region = std::move(region);
    level.degree.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        level.degree[i] = 2.0 * selfLoop[i];
    }
    std::vector<std::size_t> count(n + 1, 0);
    for (const Edge &e : merged) {
        ++count[e.u + 1];
        ++count[e.v + 1];
        level.degree[e.u] += e.w;
        level.degree[e.v] += e.w;
    }
    std::partial_sum(count.begin(), count.end(), count.begin());
    level.start = count;
    level.to.resize(count.back());
    level.weight.resize(count.back());
    std::vector<std::size_t> cursor(count.begin(), count.end() - 1);
    for (const Edge &e : merged) {
        level.to[cursor[e.u]] = e.v;
        level.weight[cursor[e.u]++] = e.w;
        level.to[cursor[e.v]] = e.u;
        level.weight[cursor[e.v]++] = e.w;
    }
    return level;
}

Partition singletons(const Level &level) {
    Partition p;
    p.of.resize(level.size());
    std::iota(p.of.begin(), p.of.end(), 0u);
    p.degree = level.degree;
    p.area = level.area;
    p.region = level.region;
    return p;
}

std::size_t countCommunities(const Partition &p) {
    std::vector<char> seen(p.of.size(), 0);
    std::size_t count = 0;
    for (const std::uint32_t c : p.of) {
        count += seen[c] == 0 ? 1 : 0;
        seen[c] = 1;
    }
    return count;
}

std::vector<std::uint32_t> denseIds(const std::vector<std::uint32_t> &of, std::size_t &count) {
    const std::uint32_t largest = of.empty() ? 0 : std::ranges::max(of);
    std::vector<std::uint32_t> id(static_cast<std::size_t>(largest) + 1, none);
    std::vector<std::uint32_t> out(of.size());
    count = 0;
    for (std::size_t i = 0; i < of.size(); ++i) {
        if (id[of[i]] == none) {
            id[of[i]] = static_cast<std::uint32_t>(count++);
        }
        out[i] = id[of[i]];
    }
    return out;
}

Level aggregate(const Level &g, const std::vector<std::uint32_t> &node, std::size_t count) {
    std::vector<double> area(count, 0.0);
    std::vector<int> region(count, 0);
    std::vector<double> selfLoop(count, 0.0);
    std::vector<Edge> edges;
    for (std::uint32_t i = 0; i < g.size(); ++i) {
        area[node[i]] += g.area[i];
        region[node[i]] = g.region[i];
        double outside = 0.0;
        for (std::size_t e = g.start[i]; e < g.start[i + 1]; ++e) {
            outside += g.weight[e];
        }
        selfLoop[node[i]] += 0.5 * (g.degree[i] - outside);
        for (std::size_t e = g.start[i]; e < g.start[i + 1]; ++e) {
            const std::uint32_t j = g.to[e];
            if (j <= i) {
                continue;
            }
            if (node[i] == node[j]) {
                selfLoop[node[i]] += g.weight[e];
            } else {
                edges.push_back(
                    {std::min(node[i], node[j]), std::max(node[i], node[j]), g.weight[e]});
            }
        }
    }
    return buildLevel(std::move(area), std::move(region), std::move(selfLoop), std::move(edges));
}

std::size_t targetCommunities(const Level &level, const ClusterOptions &opts) {
    return static_cast<std::size_t>(static_cast<double>(level.size()) /
                                    std::max(opts.cellsPerCluster, 1.0));
}

Modularity::Modularity(const Level &level, double gamma, double areaCap)
    : g(level), resolution(gamma), cap(areaCap) {
    totalDegree = std::accumulate(g.degree.begin(), g.degree.end(), 0.0);
    toCommunity.assign(g.size(), 0.0);
}

void Modularity::release() {
    for (const std::uint32_t c : touched) {
        toCommunity[c] = 0.0;
    }
    touched.clear();
}

double Modularity::gain(std::uint32_t i, std::uint32_t c, const Partition &p) const {
    return toCommunity[c] - resolution * g.degree[i] * p.degree[c] / totalDegree;
}

bool Modularity::moveNodes(Partition &p) {
    if (!(totalDegree > 0.0)) {
        return false;
    }
    std::deque<std::uint32_t> queue(g.size());
    std::iota(queue.begin(), queue.end(), 0u);
    std::vector<char> queued(g.size(), 1);
    bool moved = false;
    while (!queue.empty()) {
        const std::uint32_t i = queue.front();
        queue.pop_front();
        queued[i] = 0;
        const std::uint32_t own = p.of[i];
        p.degree[own] -= g.degree[i];
        p.area[own] -= g.area[i];
        const std::vector<std::uint32_t> &candidates = gather(i, p, [](std::uint32_t) {
            return true;
        });
        std::uint32_t best = own;
        double bestGain = gain(i, own, p);
        for (const std::uint32_t c : candidates) {
            if (c == own || p.region[c] != g.region[i] || p.area[c] + g.area[i] > cap) {
                continue;
            }
            const double candidate = gain(i, c, p);
            if (candidate > bestGain + gainEpsilon) {
                best = c;
                bestGain = candidate;
            }
        }
        release();
        p.degree[best] += g.degree[i];
        p.area[best] += g.area[i];
        p.region[best] = g.region[i];
        p.of[i] = best;
        if (best == own) {
            continue;
        }
        moved = true;
        for (std::size_t e = g.start[i]; e < g.start[i + 1]; ++e) {
            const std::uint32_t j = g.to[e];
            if (p.of[j] != best && queued[j] == 0) {
                queued[j] = 1;
                queue.push_back(j);
            }
        }
    }
    return moved;
}

Communities louvain(Level level, const ClusterOptions &opts, double areaCap) {
    Communities out;
    out.ofNode.resize(level.size());
    std::iota(out.ofNode.begin(), out.ofNode.end(), 0u);
    const std::size_t target = targetCommunities(level, opts);
    Partition p = singletons(level);
    for (std::size_t round = 0; round < opts.maxLevels; ++round) {
        Modularity modularity(level, opts.resolution, areaCap);
        const bool moved = modularity.moveNodes(p);
        ++out.levels;
        if (!moved || countCommunities(p) <= target || round + 1 == opts.maxLevels) {
            break;
        }
        std::size_t count = 0;
        const std::vector<std::uint32_t> node = denseIds(p.of, count);
        if (count == level.size()) {
            break;
        }
        Level next = aggregate(level, node, count);
        for (std::uint32_t &c : out.ofNode) {
            c = node[c];
        }
        level = std::move(next);
        p = singletons(level);
    }
    for (std::uint32_t &c : out.ofNode) {
        c = p.of[c];
    }
    out.ofNode = denseIds(out.ofNode, out.count);
    return out;
}

}  // namespace ktplace::cluster
