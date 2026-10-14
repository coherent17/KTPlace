// @file ntuplace1_options.cc
// Reading NTUPlace1's options from the option store

#include "placer/ntuplace1/ntuplace1_options.h"

namespace ktplace {

namespace {

const Option<std::size_t> kLeafCells{"ntuplace1", "leaf_cells", 100};
const Option<std::size_t> kMaxLevels{"ntuplace1", "max_levels", 32};
const Option<std::size_t> kRetries{"ntuplace1", "retries", 4};
const Option<bool> kVerbose{"ntuplace1", "verbose", false};

}  // namespace

Ntuplace1Options Ntuplace1Options::load() {
    Ntuplace1Options out;
    out.leafCells = kLeafCells.value();
    out.maxLevels = kMaxLevels.value();
    out.retries = kRetries.value();
    out.verbose = kVerbose.value();
    out.rows = {resolve(kLeafCells), resolve(kMaxLevels), resolve(kRetries), resolve(kVerbose)};
    return out;
}

void Ntuplace1Options::report() const {
    reportOptions("ntuplace1", rows);
}

}  // namespace ktplace
