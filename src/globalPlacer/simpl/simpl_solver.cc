#include "globalPlacer/simpl/simpl_solver.h"

#include <cmath>
#include <oneapi/tbb/parallel_invoke.h>

namespace ktplace::simpl {

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
