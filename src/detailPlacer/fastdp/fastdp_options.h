// @file fastdp_options.h
// FastDP detailed placement options, and where they came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <vector>

namespace ktplace {

// Every option the detailed placer reads:
//
//     add_option fastdp window 2
//
// Each was a KTPLACE_DP_* environment variable. The environment is no longer
// read; a script is the only way to set them.
struct FastdpOptions {
    // How many neighbouring cells a local reorder may swap across. Zero leaves
    // reordering off, which is the setting that makes the placer a no-op.
    std::size_t window = 0;

    // The median pass. It is what removes the overlaps Abacus leaves behind, so
    // turning it off trades legality for speed.
    bool noMedian = false;

    [[nodiscard]] static FastdpOptions load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
