#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>

namespace ktplace {

struct ClusterOptions {
    static constexpr Component component = Component::Cluster;

    std::string algorithm = "none";
    double cellsPerCluster = 8.0;
    double areaLimit = 2.0;
    std::size_t maxNetSize = 32;
    double resolution = 1.0;
    std::size_t maxLevels = 8;

    static void fields(auto &self, auto &&visit) {
        visit("algorithm", self.algorithm);
        visit("cells_per_cluster", self.cellsPerCluster);
        visit("area_limit", self.areaLimit);
        visit("max_net_size", self.maxNetSize);
        visit("resolution", self.resolution);
        visit("max_levels", self.maxLevels);
    }
};

}  // namespace ktplace
