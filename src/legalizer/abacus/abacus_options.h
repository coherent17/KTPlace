// @file abacus_options.h
// Abacus legalization options, and where they came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <vector>

namespace ktplace {

// Every option the Abacus legalizer reads:
//
//     add_option abacus max_row_dist 3
//
// Set with `add_option abacus <option> <value>`; the defaults are below.
struct AbacusOptions {
    // How far a cell may be dragged along its row to make it fit. Unlimited would
    // legalize at the cost of a wirelength nobody asked for.
    std::size_t maxRowDistance = 16;

    // Empty subrows shorter than this are not worth placing into, because the
    // search cost is not repaid by the few cells they hold.
    std::size_t minSubrowSites = 1;

    // Cells may be left in a cluster rather than placed individually, when the
    // cluster is smaller than this.
    std::size_t clusterGap = 0;

    [[nodiscard]] static AbacusOptions load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
