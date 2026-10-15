// @file abacus_options.cc
// Reading the Abacus legalizer's options from the option store

#include "legalizer/abacus/abacus_options.h"

namespace ktplace {

namespace {

const Option<std::size_t> kMaxRowDist{"abacus", "max_row_dist", 0};
const Option<std::size_t> kMinSubrowSites{"abacus", "min_subrow_sites", 1};
const Option<std::size_t> kClusterGap{"abacus", "cluster_gap", 0};

}  // namespace

AbacusOptions AbacusOptions::load() {
    AbacusOptions out;
    out.maxRowDistance = kMaxRowDist.value();
    out.minSubrowSites = kMinSubrowSites.value();
    out.clusterGap = kClusterGap.value();
    out.rows = {resolve(kMaxRowDist), resolve(kMinSubrowSites), resolve(kClusterGap)};
    return out;
}

void AbacusOptions::report() const {
    reportOptions("abacus", rows);
}

}  // namespace ktplace
