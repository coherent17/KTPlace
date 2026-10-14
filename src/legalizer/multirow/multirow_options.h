// @file multirow_options.h
// Multi-row legalization options, and where they came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <vector>

namespace ktplace {

// Every option the multi-row legalizer reads:
//
//     add_option multirow frame_every 5000
struct MultirowOptions {
    // Snapshot every N cells placed. Zero offers none. The default is coarse
    // because the pass places every movable cell, not only the tall ones.
    std::size_t frameEvery = 20000;

    [[nodiscard]] static MultirowOptions load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
