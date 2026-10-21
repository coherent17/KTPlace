#pragma once

#include "cluster/kt_clusterOptions.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace ktplace::cluster {

inline constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

struct Edge {
    std::uint32_t u = 0;
    std::uint32_t v = 0;
    double w = 0.0;
};

struct Level {
    std::vector<std::size_t> start;
    std::vector<std::uint32_t> to;
    std::vector<double> weight;
    std::vector<double> degree;
    std::vector<double> area;
    std::vector<int> region;

    [[nodiscard]] std::size_t size() const {
        return area.size();
    }
};

struct Partition {
    std::vector<std::uint32_t> of;
    std::vector<double> degree;
    std::vector<double> area;
    std::vector<int> region;
};

struct Communities {
    std::vector<std::uint32_t> ofNode;
    std::size_t count = 0;
    std::size_t levels = 0;
};

[[nodiscard]] Level buildLevel(std::vector<double> area, std::vector<int> region,
                               std::vector<double> selfLoop, std::vector<Edge> edges);
[[nodiscard]] Partition singletons(const Level &level);
[[nodiscard]] std::size_t countCommunities(const Partition &p);
[[nodiscard]] std::vector<std::uint32_t> denseIds(const std::vector<std::uint32_t> &of,
                                                  std::size_t &count);
[[nodiscard]] Level aggregate(const Level &g, const std::vector<std::uint32_t> &node,
                              std::size_t count);
[[nodiscard]] std::size_t targetCommunities(const Level &level, const ClusterOptions &opts);

class Modularity {
public:
    Modularity(const Level &level, double gamma, double cap);

    bool moveNodes(Partition &p);

    [[nodiscard]] const Level &level() const {
        return g;
    }
    [[nodiscard]] double areaCap() const {
        return cap;
    }

    template <typename Admit>
    const std::vector<std::uint32_t> &gather(std::uint32_t i, const Partition &p, Admit &&admit) {
        for (std::size_t e = g.start[i]; e < g.start[i + 1]; ++e) {
            const std::uint32_t j = g.to[e];
            if (j == i || !admit(j)) {
                continue;
            }
            const std::uint32_t c = p.of[j];
            if (toCommunity[c] == 0.0) {
                touched.push_back(c);
            }
            toCommunity[c] += g.weight[e];
        }
        return touched;
    }
    void release();
    [[nodiscard]] double gain(std::uint32_t i, std::uint32_t c, const Partition &p) const;

private:
    const Level &g;
    double resolution;
    double cap;
    double totalDegree = 0.0;
    std::vector<double> toCommunity;
    std::vector<std::uint32_t> touched;
};

[[nodiscard]] Communities louvain(Level level, const ClusterOptions &opts, double areaCap);

}  // namespace ktplace::cluster
