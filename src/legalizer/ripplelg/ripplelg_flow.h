#pragma once

#include <cstddef>
#include <limits>
#include <vector>

namespace ktplace::ripplelg {

inline constexpr double unlimited = std::numeric_limits<double>::infinity();

class NetworkSimplex {
public:
    explicit NetworkSimplex(std::size_t nodes);

    void addArc(std::size_t from, std::size_t to, double capacity, double cost);
    [[nodiscard]] bool solve(std::size_t maxPivots);
    [[nodiscard]] const std::vector<double> &getPotentials() const noexcept {
        return potential;
    }
    [[nodiscard]] double getCost() const;

private:
    struct Arc {
        std::size_t from = 0;
        std::size_t to = 0;
        double capacity = 0.0;
        double cost = 0.0;
        double flow = 0.0;
    };

    [[nodiscard]] double getReducedCost(std::size_t a) const;
    void pivot(std::size_t entering, bool forward);
    void reroot(std::size_t newRoot, std::size_t oldTop, std::size_t attachTo, std::size_t arc);
    void refresh(std::size_t top, double shift);

    std::size_t numNodes;
    std::size_t root;
    std::vector<Arc> arcs;
    std::vector<std::size_t> parent;
    std::vector<std::size_t> parentArc;
    std::vector<std::size_t> depth;
    std::vector<std::vector<std::size_t>> children;
    std::vector<double> potential;
};

}  // namespace ktplace::ripplelg
