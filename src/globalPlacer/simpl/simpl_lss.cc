#include "globalPlacer/simpl/simpl_lss.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <oneapi/tbb/parallel_invoke.h>
#include <utility>

namespace ktplace::simpl {

void LinearSystems::buildB2B(const std::vector<double> &x, const std::vector<double> &y,
                             double alpha, const std::vector<double> &anchorX,
                             const std::vector<double> &anchorY) const {
    std::vector<double> px = netlist.fixedX;
    std::vector<double> py = netlist.fixedY;
    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        px[netlist.movVertex[i]] = x[i];
        py[netlist.movVertex[i]] = y[i];
    }
    tbb::parallel_invoke(
        [&] {
            buildB2BAxis(sysX, px, floorX, alpha, anchorX, true);
        },
        [&] {
            buildB2BAxis(sysY, py, floorY, alpha, anchorY, false);
        });
}

void LinearSystems::buildB2BAxis(SparseSystem &sys, const std::vector<double> &pos,
                                 double lengthFloor, double alpha,
                                 const std::vector<double> &anchor, bool vertical) const {
    const auto pinAt = [&](const NetPins &net, std::size_t q) {
        return pos[net.cells[q]] + (vertical ? net.offX[q] : net.offY[q]);
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extremes(netlist.nets.size());
    parallelFor(netlist.nets.size(), [&](std::size_t n) {
        const NetPins &net = netlist.nets[n];
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        for (std::uint32_t q = 1; q < net.cells.size(); ++q) {
            lo = pinAt(net, q) < pinAt(net, lo) ? q : lo;
            hi = pinAt(net, q) > pinAt(net, hi) ? q : hi;
        }
        extremes[n] = {lo, hi};
    });

    const auto forEachEdge = [&](std::size_t i, auto &&visit) {
        for (std::size_t e = netlist.pinStart[i]; e < netlist.pinStart[i + 1]; ++e) {
            const auto [n, q] = netlist.pinsOfCell[e];
            const NetPins &net = netlist.nets[n];
            const auto [lo, hi] = extremes[n];
            if (lo == hi) {
                continue;
            }
            const double base = net.weight / static_cast<double>(net.cells.size() - 1);
            const double own = pinAt(net, q);
            const auto edge = [&](std::uint32_t other) {
                const double there = pinAt(net, other);
                const double w = base / std::max(std::abs(own - there), lengthFloor);
                visit(net.cells[other], there, own - pos[net.cells[q]], w);
            };
            if (q == lo || q == hi) {
                for (std::uint32_t other = 0; other < net.cells.size(); ++other) {
                    if (other != q) {
                        edge(other);
                    }
                }
            } else {
                edge(lo);
                edge(hi);
            }
        }
    };

    sys.rowPtr.assign(netlist.numMovable + 1, 0);
    parallelFor(netlist.numMovable, [&](std::size_t i) {
        std::size_t count = 0;
        forEachEdge(i, [&](std::uint32_t cell, double, double, double) {
            count += netlist.varOfVertex[cell] != noVar ? 1 : 0;
        });
        sys.rowPtr[i + 1] = count;
    });
    std::partial_sum(sys.rowPtr.begin(), sys.rowPtr.end(), sys.rowPtr.begin());
    sys.col.resize(sys.rowPtr.back());
    sys.val.resize(sys.rowPtr.back());
    sys.diag.assign(netlist.numMovable, 0.0);
    sys.rhs.assign(netlist.numMovable, 0.0);

    parallelFor(netlist.numMovable, [&](std::size_t i) {
        std::size_t slot = sys.rowPtr[i];
        double diag = 0.0;
        double rhs = 0.0;
        forEachEdge(i, [&](std::uint32_t cell, double pin, double ownOffset, double w) {
            const std::uint32_t var = netlist.varOfVertex[cell];
            diag += w;
            if (var != noVar) {
                sys.col[slot] = var;
                sys.val[slot] = -w;
                ++slot;
                rhs += w * ((pin - pos[cell]) - ownOffset);
            } else {
                rhs += w * (pin - ownOffset);
            }
        });
        if (alpha > 0.0) {
            const double w =
                alpha / std::max(std::abs(pos[netlist.movVertex[i]] - anchor[i]), lengthFloor);
            diag += w;
            rhs += w * anchor[i];
        }
        sys.diag[i] = std::max(diag, 1e-12);
        sys.rhs[i] = rhs;
    });
}

std::size_t LinearSystems::buildStar() const {
    std::vector<std::uint32_t> starOf(netlist.nets.size(), noVar);
    std::size_t size = netlist.numMovable;
    for (std::size_t n = 0; n < netlist.nets.size(); ++n) {
        const std::vector<std::uint32_t> &cells = netlist.nets[n].cells;
        const bool movable = std::ranges::any_of(cells, [&](std::uint32_t c) {
            return netlist.varOfVertex[c] != noVar;
        });
        if (cells.size() >= 3 && movable) {
            starOf[n] = static_cast<std::uint32_t>(size++);
        }
    }
    tbb::parallel_invoke(
        [&] {
            buildStarAxis(sysX, starOf, size, true);
        },
        [&] {
            buildStarAxis(sysY, starOf, size, false);
        });
    return size;
}

void LinearSystems::buildStarAxis(SparseSystem &sys, const std::vector<std::uint32_t> &starOf,
                                  std::size_t size, bool vertical) const {
    struct End {
        std::uint32_t var;
        double offset;
        double fixed;
    };
    std::vector<std::vector<std::pair<std::uint32_t, double>>> rows(size);
    sys.diag.assign(size, 0.0);
    sys.rhs.assign(size, 0.0);
    const auto spring = [&](const End &a, const End &b, double w) {
        const auto pull = [&](const End &self, const End &other) {
            if (self.var == noVar) {
                return;
            }
            sys.diag[self.var] += w;
            if (other.var != noVar) {
                rows[self.var].emplace_back(other.var, -w);
                sys.rhs[self.var] += w * (other.offset - self.offset);
            } else {
                sys.rhs[self.var] += w * (other.fixed + other.offset - self.offset);
            }
        };
        pull(a, b);
        pull(b, a);
    };
    for (std::size_t n = 0; n < netlist.nets.size(); ++n) {
        const NetPins &net = netlist.nets[n];
        const std::size_t p = net.cells.size();
        if (p < 2) {
            continue;
        }
        const auto end = [&](std::size_t q) {
            const std::uint32_t c = net.cells[q];
            return End{netlist.varOfVertex[c], vertical ? net.offX[q] : net.offY[q],
                       vertical ? netlist.fixedX[c] : netlist.fixedY[c]};
        };
        if (p == 2) {
            spring(end(0), end(1), net.weight);
        } else if (starOf[n] != noVar) {
            const End star{starOf[n], 0.0, 0.0};
            const double w = net.weight * static_cast<double>(p) / static_cast<double>(p - 1);
            for (std::size_t q = 0; q < p; ++q) {
                spring(end(q), star, w);
            }
        }
    }
    sys.rowPtr.assign(size + 1, 0);
    for (std::size_t i = 0; i < size; ++i) {
        sys.rowPtr[i + 1] = sys.rowPtr[i] + rows[i].size();
        sys.diag[i] = std::max(sys.diag[i], 1e-12);
    }
    sys.col.resize(sys.rowPtr.back());
    sys.val.resize(sys.rowPtr.back());
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t slot = sys.rowPtr[i];
        for (const auto &[c, v] : rows[i]) {
            sys.col[slot] = c;
            sys.val[slot] = v;
            ++slot;
        }
    }
}

Residuals solveBothAxes(const SparseSystem &sysX, std::vector<double> &x, const SparseSystem &sysY,
                        std::vector<double> &y, double tolerance, std::size_t maxIterations,
                        const IterateObserver &observe) {
    struct Axis {
        const SparseSystem *sys = nullptr;
        std::vector<double> *sol = nullptr;
        std::vector<double> r, z, p, ap;
        double bNorm = 0.0;
        double rho = 0.0;
        double resid = 0.0;
        bool active = false;
    };

    const auto start = [](Axis &ax, const SparseSystem &sys, std::vector<double> &sol) {
        const std::size_t n = sys.size();
        ax.sys = &sys;
        ax.sol = &sol;
        ax.r.assign(n, 0.0);
        ax.z.assign(n, 0.0);
        ax.ap.assign(n, 0.0);
        sys.multiply(sol, ax.ap);
        parallelFor(n, [&](std::size_t i) {
            ax.r[i] = sys.rhs[i] - ax.ap[i];
            ax.z[i] = ax.r[i] / sys.diag[i];
        });
        ax.bNorm = std::sqrt(dot(sys.rhs, sys.rhs));
        if (!(ax.bNorm > 0.0)) {
            return;
        }
        ax.p = ax.z;
        ax.rho = dot(ax.r, ax.z);
        ax.resid = std::sqrt(dot(ax.r, ax.r)) / ax.bNorm;
        ax.active = true;
    };

    const auto step = [](Axis &ax) {
        const SparseSystem &sys = *ax.sys;
        sys.multiply(ax.p, ax.ap);
        const double pap = dot(ax.p, ax.ap);
        if (!(pap > 0.0)) {
            return false;
        }
        const double a = ax.rho / pap;
        parallelFor(sys.size(), [&](std::size_t i) {
            (*ax.sol)[i] += a * ax.p[i];
            ax.r[i] -= a * ax.ap[i];
            ax.z[i] = ax.r[i] / sys.diag[i];
        });
        ax.resid = std::sqrt(dot(ax.r, ax.r)) / ax.bNorm;
        const double rhoNext = dot(ax.r, ax.z);
        if (!(rhoNext > 0.0)) {
            return false;
        }
        const double beta = rhoNext / ax.rho;
        parallelFor(sys.size(), [&](std::size_t i) {
            ax.p[i] = ax.z[i] + beta * ax.p[i];
        });
        ax.rho = rhoNext;
        return true;
    };

    Axis ax;
    Axis ay;
    start(ax, sysX, x);
    start(ay, sysY, y);
    const auto live = [&](const Axis &a) {
        return a.active && a.resid > tolerance;
    };
    for (std::size_t it = 0; it < maxIterations && (live(ax) || live(ay)); ++it) {
        const bool stepX = live(ax);
        const bool stepY = live(ay);
        tbb::parallel_invoke(
            [&] {
                if (stepX && !step(ax)) {
                    ax.active = false;
                }
            },
            [&] {
                if (stepY && !step(ay)) {
                    ay.active = false;
                }
            });
        if (observe) {
            observe(it + 1, Residuals{ax.resid, ay.resid}, !live(ax) && !live(ay));
        }
    }
    return Residuals{ax.resid, ay.resid};
}

}  // namespace ktplace::simpl
