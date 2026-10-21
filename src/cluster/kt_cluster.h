#pragma once

#include "cluster/kt_clusterOptions.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ktplace {

class ktDM;

struct ClusterStats {
    std::size_t cells = 0;
    std::size_t clusterable = 0;
    std::size_t clusters = 0;
    std::size_t coarseCells = 0;
    std::size_t levels = 0;
    double largestArea = 0.0;
    double seconds = 0.0;
};

class ktCluster {
public:
    explicit ktCluster(ClusterOptions options);
    ~ktCluster();
    ktCluster(ktCluster &&) noexcept;
    ktCluster &operator=(ktCluster &&) noexcept;

    ClusterStats cluster(ktDM &db);
    void decluster(ktDM &db);
    void expand(const std::vector<float> &coarseX, const std::vector<float> &coarseY,
                std::vector<float> &x, std::vector<float> &y) const;

    [[nodiscard]] bool clustered() const;
    [[nodiscard]] const std::vector<std::uint32_t> &clusterOf() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
