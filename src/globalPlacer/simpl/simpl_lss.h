#pragma once

#include "globalPlacer/simpl/simpl_parallel.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

namespace ktplace::simpl {

inline constexpr std::uint32_t noVar = std::numeric_limits<std::uint32_t>::max();

struct PinRef {
    std::uint32_t net = 0;
    std::uint32_t pin = 0;
};

struct NetPins {
    std::vector<std::uint32_t> cells;
    std::vector<double> offX;
    std::vector<double> offY;
    double weight = 1.0;
};

struct Netlist {
    std::size_t numMovable = 0;
    std::vector<std::uint32_t> movVertex;
    std::vector<std::uint32_t> varOfVertex;
    std::vector<double> fixedX;
    std::vector<double> fixedY;
    std::vector<NetPins> nets;
    std::vector<std::size_t> pinStart;
    std::vector<PinRef> pinsOfCell;
};

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

struct LinearSystems {
    const Netlist &netlist;
    SparseSystem &sysX;
    SparseSystem &sysY;
    double floorX;
    double floorY;

    void buildB2B(const std::vector<double> &x, const std::vector<double> &y, double alpha,
                  const std::vector<double> &anchorX, const std::vector<double> &anchorY) const;
    std::size_t buildStar() const;

private:
    void buildB2BAxis(SparseSystem &sys, const std::vector<double> &pos, double lengthFloor,
                      double alpha, const std::vector<double> &anchor, bool vertical) const;
    void buildStarAxis(SparseSystem &sys, const std::vector<std::uint32_t> &starOf,
                       std::size_t size, bool vertical) const;
};

Residuals solveBothAxes(const SparseSystem &sysX, std::vector<double> &x, const SparseSystem &sysY,
                        std::vector<double> &y, double tolerance, std::size_t maxIterations,
                        const IterateObserver &observe);

}  // namespace ktplace::simpl
