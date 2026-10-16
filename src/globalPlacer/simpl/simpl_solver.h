#pragma once

#include "globalPlacer/simpl/simpl_parallel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ktplace::simpl {

struct SparseSystem {
    std::vector<std::size_t> rowPtr;
    std::vector<std::uint32_t> col;
    std::vector<double> val;
    std::vector<double> diag;
    std::vector<double> rhs;

    [[nodiscard]] std::size_t size() const {
        return diag.size();
    }

    void multiply(const std::vector<double> &in, std::vector<double> &out) const {
        parallelFor(size(), [&](std::size_t i) {
            double sum = diag[i] * in[i];
            for (std::size_t e = rowPtr[i]; e < rowPtr[i + 1]; ++e) {
                sum += val[e] * in[col[e]];
            }
            out[i] = sum;
        });
    }
};

struct Residuals {
    double x = 0.0;
    double y = 0.0;
};

using IterateObserver =
    std::function<void(std::size_t iterate, const Residuals &residuals, bool last)>;

Residuals solveBothAxes(const SparseSystem &sysX, std::vector<double> &x, const SparseSystem &sysY,
                        std::vector<double> &y, double tolerance, std::size_t maxIterations,
                        const IterateObserver &observe);

}  // namespace ktplace::simpl
