// @file ntuplace1_options.h
// NTUPlace1's options, and where they came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ktplace {

// Every option NTUPlace1 reads:
//
//     add_option ntuplace1 leaf_cells 64
//
// Each was a KTPLACE_NTU_* environment variable. The environment is no longer
// read; a script is the only way to set them.
struct Ntuplace1Options {
    // When to stop subdividing. A leaf holds this many cells, at most.
    std::size_t leafCells = 100;

    // A cap on the recursion, as a backstop for a region that will not split.
    std::size_t maxLevels = 32;

    // How many times a cut may be retried with a looser imbalance before the
    // region is left whole.
    std::size_t retries = 4;

    // Log every accepted cut and its split.
    bool verbose = false;

    [[nodiscard]] static Ntuplace1Options load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
