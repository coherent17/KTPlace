// @file simpl_options.h
// SimPL's options

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <vector>

namespace ktplace {

struct SimplOptions {
    int grid = 0;
    double cgTol = 1e-4;
    std::size_t initIters = 15;
    std::size_t iters = 100;
    std::size_t seed = 20240607;
    bool fences = true;
    double density = 1.0;
    double alphaBase = 0.01;

    [[nodiscard]] static SimplOptions load();

    void report() const;

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
