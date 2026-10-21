#include "cluster/kt_leiden.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

namespace ktplace::cluster {

namespace {

constexpr double gainEpsilon = 1e-12;

Partition parentPartition(const Level &next, const std::vector<std::uint32_t> &node,
                          const Partition &coarse, std::size_t count) {
    std::vector<std::uint32_t> parent(count, 0);
    for (std::size_t i = 0; i < node.size(); ++i) {
        parent[node[i]] = coarse.of[i];
    }
    std::size_t parents = 0;
    const std::vector<std::uint32_t> dense = denseIds(parent, parents);
    Partition start = singletons(next);
    std::ranges::fill(start.degree, 0.0);
    std::ranges::fill(start.area, 0.0);
    for (std::uint32_t k = 0; k < count; ++k) {
        start.of[k] = dense[k];
        start.degree[dense[k]] += next.degree[k];
        start.area[dense[k]] += next.area[k];
        start.region[dense[k]] = next.region[k];
    }
    return start;
}

}  // namespace

Partition refine(Modularity &modularity, const Partition &coarse) {
    const Level &g = modularity.level();
    Partition r = singletons(g);
    std::vector<std::uint32_t> members(g.size(), 1);
    for (std::uint32_t i = 0; i < g.size(); ++i) {
        if (members[r.of[i]] != 1 || r.of[i] != i) {
            continue;
        }
        r.degree[i] -= g.degree[i];
        r.area[i] -= g.area[i];
        const std::vector<std::uint32_t> &candidates =
            modularity.gather(i, r, [&](std::uint32_t j) {
                return coarse.of[j] == coarse.of[i];
            });
        std::uint32_t best = i;
        double bestGain = 0.0;
        for (const std::uint32_t c : candidates) {
            if (c == i || r.area[c] + g.area[i] > modularity.areaCap()) {
                continue;
            }
            const double candidate = modularity.gain(i, c, r);
            if (candidate > bestGain + gainEpsilon) {
                best = c;
                bestGain = candidate;
            }
        }
        modularity.release();
        r.degree[best] += g.degree[i];
        r.area[best] += g.area[i];
        r.of[i] = best;
        if (best != i) {
            --members[i];
            ++members[best];
        }
    }
    return r;
}

Communities leiden(Level level, const ClusterOptions &opts, double areaCap) {
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
        const Partition refined = refine(modularity, p);
        std::size_t count = 0;
        const std::vector<std::uint32_t> node = denseIds(refined.of, count);
        if (count == level.size()) {
            break;
        }
        Level next = aggregate(level, node, count);
        Partition start = parentPartition(next, node, p, count);
        for (std::uint32_t &c : out.ofNode) {
            c = node[c];
        }
        level = std::move(next);
        p = std::move(start);
    }
    for (std::uint32_t &c : out.ofNode) {
        c = p.of[c];
    }
    out.ofNode = denseIds(out.ofNode, out.count);
    return out;
}

}  // namespace ktplace::cluster
