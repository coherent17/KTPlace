#include "globalPlacer/kraftwerk2/kraftwerk2_potential.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

namespace ktplace::kraftwerk2 {

namespace {

struct UnitCircle {
    std::vector<double> cosine;
    std::vector<double> sine;
};

UnitCircle getUnitCircle(std::size_t halfTurnSteps) {
    std::size_t levels = 0;
    while ((std::size_t{1} << levels) < halfTurnSteps) {
        ++levels;
    }
    std::vector<double> halfCos(levels + 1);
    std::vector<double> halfSin(levels + 1);
    halfCos[0] = -1.0;
    halfSin[0] = 0.0;
    if (levels >= 1) {
        halfCos[1] = 0.0;
        halfSin[1] = 1.0;
    }
    for (std::size_t k = 2; k <= levels; ++k) {
        halfCos[k] = std::sqrt((1.0 + halfCos[k - 1]) / 2.0);
        halfSin[k] = halfSin[k - 1] / (2.0 * halfCos[k]);
    }
    UnitCircle circle;
    const std::size_t steps = 2 * halfTurnSteps;
    circle.cosine.resize(steps);
    circle.sine.resize(steps);
    for (std::size_t m = 0; m < steps; ++m) {
        double c = 1.0;
        double s = 0.0;
        for (std::size_t bit = 0; bit <= levels; ++bit) {
            if (((m >> bit) & 1U) == 0) {
                continue;
            }
            const double hc = halfCos[levels - bit];
            const double hs = halfSin[levels - bit];
            const double nextC = c * hc - s * hs;
            s = s * hc + c * hs;
            c = nextC;
        }
        circle.cosine[m] = c;
        circle.sine[m] = s;
    }
    return circle;
}

template <typename Body>
void forEachLine(std::size_t n, Body body) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 1),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t line = range.begin(); line < range.end(); ++line) {
                              body(line);
                          }
                      });
}

void analyzeRows(std::size_t n, const std::vector<double> &in, const std::vector<double> &basis,
                 std::vector<double> &out) {
    forEachLine(n, [&](std::size_t j) {
        for (std::size_t u = 0; u < n; ++u) {
            double sum = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                sum += in[j * n + i] * basis[u * n + i];
            }
            out[j * n + u] = sum;
        }
    });
}

void analyzeColumns(std::size_t n, const std::vector<double> &in, const std::vector<double> &basis,
                    std::vector<double> &out) {
    forEachLine(n, [&](std::size_t v) {
        std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(v * n), n, 0.0);
        for (std::size_t j = 0; j < n; ++j) {
            const double weight = basis[v * n + j];
            for (std::size_t i = 0; i < n; ++i) {
                out[v * n + i] += weight * in[j * n + i];
            }
        }
    });
}

void synthesizeRows(std::size_t n, const std::vector<double> &in, const std::vector<double> &basis,
                    std::vector<double> &out) {
    forEachLine(n, [&](std::size_t j) {
        std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(j * n), n, 0.0);
        for (std::size_t u = 0; u < n; ++u) {
            const double weight = in[j * n + u];
            for (std::size_t i = 0; i < n; ++i) {
                out[j * n + i] += weight * basis[u * n + i];
            }
        }
    });
}

void synthesizeColumns(std::size_t n, const std::vector<double> &in,
                       const std::vector<double> &basis, std::vector<double> &out) {
    forEachLine(n, [&](std::size_t j) {
        std::fill_n(out.begin() + static_cast<std::ptrdiff_t>(j * n), n, 0.0);
        for (std::size_t v = 0; v < n; ++v) {
            const double weight = basis[v * n + j];
            for (std::size_t i = 0; i < n; ++i) {
                out[j * n + i] += weight * in[v * n + i];
            }
        }
    });
}

}  // namespace

std::size_t getPowerOfTwoAtLeast(std::size_t value) {
    std::size_t power = 1;
    while (power < value) {
        power *= 2;
    }
    return power;
}

Potential::Potential(const BBox &region, std::size_t bins)
    : area(region),
      n(getPowerOfTwoAtLeast(std::max<std::size_t>(bins, 2))),
      binWidth(region.getWidth() / static_cast<double>(n)),
      binHeight(region.getHeight() / static_cast<double>(n)),
      cosBasis(n * n),
      sinBasis(n * n),
      gradX(n * n, 0.0),
      gradY(n * n, 0.0) {
    const UnitCircle circle = getUnitCircle(2 * n);
    for (std::size_t k = 0; k < n; ++k) {
        for (std::size_t i = 0; i < n; ++i) {
            const std::size_t step = (k * (2 * i + 1)) % (4 * n);
            cosBasis[k * n + i] = circle.cosine[step];
            sinBasis[k * n + i] = circle.sine[step];
        }
    }
}

void Potential::solve(const std::vector<double> &charge) {
    std::vector<double> scratch(n * n);
    std::vector<double> coefficients(n * n);
    analyzeRows(n, charge, cosBasis, scratch);
    analyzeColumns(n, scratch, cosBasis, coefficients);

    const double size = static_cast<double>(n);
    const double lengthX = area.getWidth();
    const double lengthY = area.getHeight();
    std::vector<double> towardX(n * n);
    std::vector<double> towardY(n * n);
    for (std::size_t v = 0; v < n; ++v) {
        for (std::size_t u = 0; u < n; ++u) {
            const std::size_t k = v * n + u;
            if (u == 0 && v == 0) {
                towardX[k] = 0.0;
                towardY[k] = 0.0;
                continue;
            }
            const double scale = (u == 0 ? 1.0 : 2.0) * (v == 0 ? 1.0 : 2.0) / (size * size);
            const double omegaX = std::numbers::pi * static_cast<double>(u) / lengthX;
            const double omegaY = std::numbers::pi * static_cast<double>(v) / lengthY;
            const double phi = scale * coefficients[k] / (omegaX * omegaX + omegaY * omegaY);
            towardX[k] = -phi * omegaX;
            towardY[k] = -phi * omegaY;
        }
    }
    synthesizeColumns(n, towardX, cosBasis, scratch);
    synthesizeRows(n, scratch, sinBasis, gradX);
    synthesizeColumns(n, towardY, sinBasis, scratch);
    synthesizeRows(n, scratch, cosBasis, gradY);
}

Point Potential::getGradient(const Point &p) const {
    const double last = static_cast<double>(n - 1);
    const double fx = std::clamp((p.x - area.lowerLeft.x) / binWidth - 0.5, 0.0, last);
    const double fy = std::clamp((p.y - area.lowerLeft.y) / binHeight - 0.5, 0.0, last);
    const auto i0 = static_cast<std::size_t>(fx);
    const auto j0 = static_cast<std::size_t>(fy);
    const std::size_t i1 = std::min(i0 + 1, n - 1);
    const std::size_t j1 = std::min(j0 + 1, n - 1);
    const double tx = fx - static_cast<double>(i0);
    const double ty = fy - static_cast<double>(j0);
    const auto blend = [&](const std::vector<double> &grid) {
        const double bottom = grid[j0 * n + i0] * (1.0 - tx) + grid[j0 * n + i1] * tx;
        const double top = grid[j1 * n + i0] * (1.0 - tx) + grid[j1 * n + i1] * tx;
        return bottom * (1.0 - ty) + top * ty;
    };
    return Point(blend(gradX), blend(gradY));
}

}  // namespace ktplace::kraftwerk2
