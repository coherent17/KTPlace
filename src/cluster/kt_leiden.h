#pragma once

#include "cluster/kt_clusterOptions.h"
#include "cluster/kt_louvain.h"

namespace ktplace::cluster {

[[nodiscard]] Partition refine(Modularity &modularity, const Partition &coarse);

[[nodiscard]] Communities leiden(Level level, const ClusterOptions &opts, double areaCap);

}  // namespace ktplace::cluster
