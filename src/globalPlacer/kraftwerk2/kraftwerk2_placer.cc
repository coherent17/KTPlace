#include "globalPlacer/kraftwerk2/kraftwerk2_placer.h"

#include "datamodel/kt_hpwl.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_potential.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_invoke.h>
#include <string>
#include <vector>

namespace ktplace::kraftwerk2 {

namespace {

constexpr std::size_t grain = 4096;

template <typename Body>
void parallelFor(std::size_t n, Body body) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, grain),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t i = range.begin(); i < range.end(); ++i) {
                              body(i);
                          }
                      });
}

template <typename Term>
double getSum(std::size_t n, Term term) {
    const std::size_t chunks = (n + grain - 1) / grain;
    std::vector<double> partial(chunks, 0.0);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, chunks, 1),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t c = range.begin(); c < range.end(); ++c) {
                              const std::size_t end = std::min(n, (c + 1) * grain);
                              double sum = 0.0;
                              for (std::size_t i = c * grain; i < end; ++i) {
                                  sum += term(i);
                              }
                              partial[c] = sum;
                          }
                      });
    return std::accumulate(partial.begin(), partial.end(), 0.0);
}

double getDot(const std::vector<double> &a, const std::vector<double> &b) {
    return getSum(a.size(), [&](std::size_t i) {
        return a[i] * b[i];
    });
}

struct System {
    std::vector<std::size_t> rowStart;
    std::vector<std::uint32_t> col;
    std::vector<double> val;
    std::vector<double> diag;
    std::vector<double> rhs;

    void multiply(const std::vector<double> &in, std::vector<double> &out) const {
        parallelFor(diag.size(), [&](std::size_t i) {
            double sum = diag[i] * in[i];
            for (std::size_t e = rowStart[i]; e < rowStart[i + 1]; ++e) {
                sum += val[e] * in[col[e]];
            }
            out[i] = sum;
        });
    }
};

struct PinRef {
    std::uint32_t net = 0;
    std::uint32_t pin = 0;
};

void solve(const System &sys, std::vector<double> &x, double tolerance, std::size_t maxIterations) {
    const std::size_t m = x.size();
    std::vector<double> r(m);
    std::vector<double> z(m);
    std::vector<double> ap(m);
    sys.multiply(x, ap);
    parallelFor(m, [&](std::size_t i) {
        r[i] = sys.rhs[i] - ap[i];
        z[i] = r[i] / sys.diag[i];
    });
    const double rhsNorm = std::sqrt(getDot(sys.rhs, sys.rhs));
    if (!(rhsNorm > 0.0)) {
        return;
    }
    std::vector<double> p = z;
    double rho = getDot(r, z);
    double residual = std::sqrt(getDot(r, r)) / rhsNorm;
    for (std::size_t it = 0; it < maxIterations && residual > tolerance; ++it) {
        sys.multiply(p, ap);
        const double pap = getDot(p, ap);
        if (!(pap > 0.0)) {
            return;
        }
        const double alpha = rho / pap;
        parallelFor(m, [&](std::size_t i) {
            x[i] += alpha * p[i];
            r[i] -= alpha * ap[i];
            z[i] = r[i] / sys.diag[i];
        });
        residual = std::sqrt(getDot(r, r)) / rhsNorm;
        const double next = getDot(r, z);
        if (!(next > 0.0)) {
            return;
        }
        const double beta = next / rho;
        parallelFor(m, [&](std::size_t i) {
            p[i] = z[i] + beta * p[i];
        });
        rho = next;
    }
}

struct BinGrid {
    BBox region;
    std::size_t nx = 1;
    std::size_t ny = 1;
    double binWidth = 1.0;
    double binHeight = 1.0;

    BinGrid(const BBox &area, std::size_t columns, std::size_t rows)
        : region(area),
          nx(columns),
          ny(rows),
          binWidth(area.getWidth() / static_cast<double>(columns)),
          binHeight(area.getHeight() / static_cast<double>(rows)) {}

    [[nodiscard]] std::size_t size() const noexcept {
        return nx * ny;
    }
    [[nodiscard]] double getBinArea() const noexcept {
        return binWidth * binHeight;
    }

    [[nodiscard]] std::size_t getIndex(double v, double origin, double size,
                                       std::size_t count) const {
        return std::min(count - 1, static_cast<std::size_t>(std::max(0.0, (v - origin) / size)));
    }

    [[nodiscard]] BBox getClipped(const BBox &box) const {
        return BBox(Point(std::max(box.lowerLeft.x, region.lowerLeft.x),
                          std::max(box.lowerLeft.y, region.lowerLeft.y)),
                    Point(std::min(box.upperRight.x, region.upperRight.x),
                          std::min(box.upperRight.y, region.upperRight.y)));
    }

    template <typename Add>
    void rasterizeRow(const BBox &clipped, std::size_t j, Add add) const {
        const double by0 = region.lowerLeft.y + static_cast<double>(j) * binHeight;
        const double h =
            std::min(clipped.upperRight.y, by0 + binHeight) - std::max(clipped.lowerLeft.y, by0);
        if (!(h > 0.0)) {
            return;
        }
        const std::size_t i0 = getIndex(clipped.lowerLeft.x, region.lowerLeft.x, binWidth, nx);
        const std::size_t i1 = getIndex(clipped.upperRight.x, region.lowerLeft.x, binWidth, nx);
        for (std::size_t i = i0; i <= i1; ++i) {
            const double bx0 = region.lowerLeft.x + static_cast<double>(i) * binWidth;
            const double w =
                std::min(clipped.upperRight.x, bx0 + binWidth) - std::max(clipped.lowerLeft.x, bx0);
            if (w > 0.0) {
                add(j * nx + i, w * h);
            }
        }
    }

    template <typename Add>
    void rasterize(const BBox &box, Add add) const {
        const BBox clipped = getClipped(box);
        if (!(clipped.upperRight.x > clipped.lowerLeft.x) ||
            !(clipped.upperRight.y > clipped.lowerLeft.y)) {
            return;
        }
        const std::size_t j0 = getIndex(clipped.lowerLeft.y, region.lowerLeft.y, binHeight, ny);
        const std::size_t j1 = getIndex(clipped.upperRight.y, region.lowerLeft.y, binHeight, ny);
        for (std::size_t j = j0; j <= j1; ++j) {
            rasterizeRow(clipped, j, add);
        }
    }

    template <typename BoxOf, typename WeightOf>
    void accumulate(std::size_t count, BoxOf boxOf, WeightOf weightOf,
                    std::vector<double> &grid) const {
        std::vector<BBox> clipped(count, BBox(Point(0.0, 0.0), Point(0.0, 0.0)));
        std::vector<std::size_t> firstRow(count, 0);
        std::vector<std::size_t> lastRow(count, 0);
        std::vector<char> inside(count, 0);
        parallelFor(count, [&](std::size_t b) {
            clipped[b] = getClipped(boxOf(b));
            if (clipped[b].upperRight.x > clipped[b].lowerLeft.x &&
                clipped[b].upperRight.y > clipped[b].lowerLeft.y) {
                inside[b] = 1;
                firstRow[b] = getIndex(clipped[b].lowerLeft.y, region.lowerLeft.y, binHeight, ny);
                lastRow[b] = getIndex(clipped[b].upperRight.y, region.lowerLeft.y, binHeight, ny);
            }
        });
        std::vector<std::size_t> rowStart(ny + 1, 0);
        for (std::size_t b = 0; b < count; ++b) {
            if (inside[b] != 0) {
                for (std::size_t j = firstRow[b]; j <= lastRow[b]; ++j) {
                    ++rowStart[j + 1];
                }
            }
        }
        std::partial_sum(rowStart.begin(), rowStart.end(), rowStart.begin());
        std::vector<std::size_t> members(rowStart.back());
        std::vector<std::size_t> cursor(rowStart.begin(), rowStart.end() - 1);
        for (std::size_t b = 0; b < count; ++b) {
            if (inside[b] != 0) {
                for (std::size_t j = firstRow[b]; j <= lastRow[b]; ++j) {
                    members[cursor[j]++] = b;
                }
            }
        }
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, ny, 1),
                          [&](const tbb::blocked_range<std::size_t> &range) {
                              for (std::size_t j = range.begin(); j < range.end(); ++j) {
                                  for (std::size_t e = rowStart[j]; e < rowStart[j + 1]; ++e) {
                                      const std::size_t b = members[e];
                                      const double weight = weightOf(b);
                                      rasterizeRow(clipped[b], j,
                                                   [&](std::size_t bin, double area) {
                                                       grid[bin] += weight * area;
                                                   });
                                  }
                              }
                          });
    }
};

struct Density {
    std::vector<double> charge;
    double overlap = 0.0;
};

struct Progress {
    std::size_t iteration = 0;
    double hpwl = 0.0;
    double overlap = 0.0;
    double movement = 0.0;
    double weightScale = 0.0;
};

}  // namespace

class Kraftwerk2Placer::Impl {
public:
    explicit Impl(Design &design) : d(design) {}

    void place(const Kraftwerk2Options &options);

private:
    [[nodiscard]] double getPinCoordinate(const NetPin &pin, bool horizontal) const;
    void indexPins();
    void buildSystem(bool horizontal, bool withRhs, System &sys) const;
    void placeInitially();
    void clampToDie();
    [[nodiscard]] double getHpwl() const;
    [[nodiscard]] double getOverlap() const;
    [[nodiscard]] std::vector<double> getSupply(const std::vector<double> &demand) const;
    [[nodiscard]] Density getDensity() const;
    void writeFrame(const std::string &note, std::size_t step, double overlap) const;

    Design &d;
    Kraftwerk2Options opts;
    std::unique_ptr<Potential> potential;
    std::unique_ptr<BinGrid> densityGrid;
    std::unique_ptr<BinGrid> overlapGrid;
    std::vector<double> moduleDensity;
    std::vector<double> fixedDemand;
    std::vector<double> fixedCover;
    double supplyDensity = 1.0;
    std::vector<std::size_t> netPinStart;
    std::vector<std::size_t> cellPinStart;
    std::vector<PinRef> cellPins;
    double floorX = 1.0;
    double floorY = 1.0;
    double hpwlInitial = 0.0;
};

double Kraftwerk2Placer::Impl::getPinCoordinate(const NetPin &pin, bool horizontal) const {
    if (pin.slot == noSlot) {
        return horizontal ? pin.fixedPosition.x : pin.fixedPosition.y;
    }
    return horizontal ? d.cells.x[pin.slot] + pin.offset.x : d.cells.y[pin.slot] + pin.offset.y;
}

void Kraftwerk2Placer::Impl::indexPins() {
    const std::size_t m = d.cells.size();
    netPinStart.assign(d.nets.size() + 1, 0);
    cellPinStart.assign(m + 1, 0);
    for (std::size_t k = 0; k < d.nets.size(); ++k) {
        netPinStart[k + 1] = netPinStart[k] + d.nets[k].size();
        for (const NetPin &pin : d.nets[k]) {
            if (pin.slot != noSlot) {
                ++cellPinStart[pin.slot + 1];
            }
        }
    }
    std::partial_sum(cellPinStart.begin(), cellPinStart.end(), cellPinStart.begin());
    cellPins.resize(cellPinStart.back());
    std::vector<std::size_t> cursor(cellPinStart.begin(), cellPinStart.end() - 1);
    for (std::size_t k = 0; k < d.nets.size(); ++k) {
        const auto pins = d.nets[k];
        for (std::size_t q = 0; q < pins.size(); ++q) {
            if (pins[q].slot != noSlot) {
                cellPins[cursor[pins[q].slot]++] = PinRef{.net = static_cast<std::uint32_t>(k),
                                                          .pin = static_cast<std::uint32_t>(q)};
            }
        }
    }
}

void Kraftwerk2Placer::Impl::buildSystem(bool horizontal, bool withRhs, System &sys) const {
    const std::size_t m = d.cells.size();
    const std::size_t numNets = d.nets.size();
    std::vector<double> coord(netPinStart.back());
    std::vector<std::uint32_t> lowest(numNets, 0);
    std::vector<std::uint32_t> highest(numNets, 0);
    parallelFor(numNets, [&](std::size_t k) {
        const auto pins = d.nets[k];
        double *at = coord.data() + netPinStart[k];
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        for (std::uint32_t q = 0; q < pins.size(); ++q) {
            at[q] = getPinCoordinate(pins[q], horizontal);
            lo = at[q] < at[lo] ? q : lo;
            hi = at[q] >= at[hi] ? q : hi;
        }
        lowest[k] = lo;
        highest[k] = hi;
    });
    const double lengthFloor = horizontal ? floorX : floorY;
    const auto forEachEdge = [&](std::size_t cell, auto &&visit) {
        for (std::size_t e = cellPinStart[cell]; e < cellPinStart[cell + 1]; ++e) {
            const auto [k, q] = cellPins[e];
            const auto pins = d.nets[k];
            const std::size_t p = pins.size();
            if (p < 2) {
                continue;
            }
            const double *at = coord.data() + netPinStart[k];
            const double base = d.netWeight[k] * 2.0 / static_cast<double>(p - 1);
            const double ownOffset = horizontal ? pins[q].offset.x : pins[q].offset.y;
            const auto edge = [&](std::size_t other) {
                const NetPin &pin = pins[other];
                if (pin.slot == cell) {
                    return;
                }
                const double w = base / std::max(std::abs(at[q] - at[other]), lengthFloor);
                const double otherOffset = horizontal ? pin.offset.x : pin.offset.y;
                visit(pin.slot, w,
                      pin.slot == noSlot ? at[other] - ownOffset : otherOffset - ownOffset);
            };
            if (q == lowest[k] || q == highest[k]) {
                for (std::size_t other = 0; other < p; ++other) {
                    if (other != q) {
                        edge(other);
                    }
                }
            } else {
                edge(lowest[k]);
                edge(highest[k]);
            }
        }
    };
    sys.rowStart.assign(m + 1, 0);
    parallelFor(m, [&](std::size_t i) {
        std::size_t count = 0;
        forEachEdge(i, [&](std::size_t slot, double, double) {
            count += slot != noSlot ? 1 : 0;
        });
        sys.rowStart[i + 1] = count;
    });
    std::partial_sum(sys.rowStart.begin(), sys.rowStart.end(), sys.rowStart.begin());
    sys.col.resize(sys.rowStart.back());
    sys.val.resize(sys.rowStart.back());
    sys.diag.assign(m, 0.0);
    sys.rhs.assign(m, 0.0);
    parallelFor(m, [&](std::size_t i) {
        std::size_t entry = sys.rowStart[i];
        double diag = 0.0;
        double rhs = 0.0;
        forEachEdge(i, [&](std::size_t slot, double w, double pull) {
            diag += w;
            rhs += w * pull;
            if (slot != noSlot) {
                sys.col[entry] = static_cast<std::uint32_t>(slot);
                sys.val[entry] = -w;
                ++entry;
            }
        });
        sys.diag[i] = diag;
        sys.rhs[i] = withRhs ? rhs : 0.0;
    });
}

void Kraftwerk2Placer::Impl::clampToDie() {
    Cells &cells = d.cells;
    parallelFor(cells.size(), [&](std::size_t i) {
        cells.x[i] = std::clamp(cells.x[i], d.die.lowerLeft.x,
                                std::max(d.die.lowerLeft.x, d.die.upperRight.x - cells.width[i]));
        cells.y[i] = std::clamp(cells.y[i], d.die.lowerLeft.y,
                                std::max(d.die.lowerLeft.y, d.die.upperRight.y - cells.height[i]));
    });
}

void Kraftwerk2Placer::Impl::placeInitially() {
    Cells &cells = d.cells;
    const Point center = d.die.getCenter();
    for (std::size_t i = 0; i < cells.size(); ++i) {
        cells.x[i] = center.x - cells.width[i] / 2.0;
        cells.y[i] = center.y - cells.height[i] / 2.0;
    }
    for (std::size_t it = 0; it < opts.initIters; ++it) {
        System sysX;
        System sysY;
        tbb::parallel_invoke(
            [&] {
                buildSystem(true, true, sysX);
            },
            [&] {
                buildSystem(false, true, sysY);
            });
        for (System *sys : {&sysX, &sysY}) {
            for (double &value : sys->diag) {
                value = std::max(value, 1e-12);
            }
        }
        tbb::parallel_invoke(
            [&] {
                solve(sysX, cells.x, opts.cgTol, opts.cgMaxIter);
            },
            [&] {
                solve(sysY, cells.y, opts.cgTol, opts.cgMaxIter);
            });
        clampToDie();
    }
}

double Kraftwerk2Placer::Impl::getHpwl() const {
    return getNetHpwl(
        d.nets.size(),
        [this](std::size_t n) {
            return d.nets[n];
        },
        [this](const NetPin &pin) {
            return Point(getPinCoordinate(pin, true), getPinCoordinate(pin, false));
        });
}

std::vector<double> Kraftwerk2Placer::Impl::getSupply(const std::vector<double> &demand) const {
    const std::size_t n = potential->getBins();
    const double binArea = potential->getBinWidth() * potential->getBinHeight();
    const double total = std::accumulate(demand.begin(), demand.end(), 0.0);
    const double chipArea = static_cast<double>(n * n) * binArea;
    const double density = opts.targetDensity;
    if (!(density > 0.0) || density * chipArea <= total) {
        return std::vector<double>(n * n, total / static_cast<double>(n * n));
    }

    constexpr std::size_t unreached = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> ring(n * n, unreached);
    std::vector<std::size_t> queue;
    for (std::size_t b = 0; b < demand.size(); ++b) {
        if (demand[b] > 0.0) {
            ring[b] = 0;
            queue.push_back(b);
        }
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        const std::size_t b = queue[head];
        const std::size_t i = b % n;
        const std::size_t j = b / n;
        for (std::size_t nj = j == 0 ? 0 : j - 1; nj <= std::min(n - 1, j + 1); ++nj) {
            for (std::size_t ni = i == 0 ? 0 : i - 1; ni <= std::min(n - 1, i + 1); ++ni) {
                const std::size_t next = nj * n + ni;
                if (ring[next] == unreached) {
                    ring[next] = ring[b] + 1;
                    queue.push_back(next);
                }
            }
        }
    }
    std::vector<double> capacity;
    for (const std::size_t r : ring) {
        if (r >= capacity.size()) {
            capacity.resize(r + 1, 0.0);
        }
        capacity[r] += density * binArea;
    }
    double filled = 0.0;
    std::size_t last = 0;
    while (filled + capacity[last] < total) {
        filled += capacity[last];
        ++last;
    }
    const double fraction = (total - filled) / capacity[last];
    std::vector<double> supply(n * n, 0.0);
    for (std::size_t b = 0; b < supply.size(); ++b) {
        if (ring[b] < last) {
            supply[b] = density * binArea;
        } else if (ring[b] == last) {
            supply[b] = fraction * density * binArea;
        }
    }
    return supply;
}

Density Kraftwerk2Placer::Impl::getDensity() const {
    const std::size_t n = potential->getBins();
    const double binArea = potential->getBinWidth() * potential->getBinHeight();
    std::vector<double> demand = fixedDemand;
    const Cells &cells = d.cells;
    densityGrid->accumulate(
        cells.size(),
        [&](std::size_t i) {
            return BBox::of(cells.x[i], cells.y[i], cells.width[i], cells.height[i]);
        },
        [&](std::size_t i) {
            return moduleDensity[i];
        },
        demand);
    const std::vector<double> supply = getSupply(demand);
    Density out;
    out.charge.resize(n * n);
    for (std::size_t b = 0; b < out.charge.size(); ++b) {
        out.charge[b] = (demand[b] - supply[b]) / binArea;
    }
    out.overlap = getOverlap();
    return out;
}

double Kraftwerk2Placer::Impl::getOverlap() const {
    std::vector<double> covered = fixedCover;
    const Cells &cells = d.cells;
    overlapGrid->accumulate(
        cells.size(),
        [&](std::size_t i) {
            return BBox::of(cells.x[i], cells.y[i], cells.width[i], cells.height[i]);
        },
        [](std::size_t) {
            return 1.0;
        },
        covered);
    const double binArea = overlapGrid->getBinArea();
    double modules = 0.0;
    double unionArea = 0.0;
    for (const double area : covered) {
        modules += area;
        unionArea += std::min(binArea, area);
    }
    return modules > 0.0 ? 1.0 - unionArea / modules : 0.0;
}

void Kraftwerk2Placer::Impl::writeFrame(const std::string &note, std::size_t step,
                                        double overlap) const {
    if (!d.onFrame) {
        return;
    }
    const Cells &cells = d.cells;
    std::vector<float> fx(d.vertexX.size());
    std::vector<float> fy(d.vertexY.size());
    for (std::size_t v = 0; v < fx.size(); ++v) {
        fx[v] = static_cast<float>(d.vertexX[v]);
        fy[v] = static_cast<float>(d.vertexY[v]);
    }
    for (std::size_t i = 0; i < cells.size(); ++i) {
        fx[cells.id[i]] = static_cast<float>(cells.x[i]);
        fy[cells.id[i]] = static_cast<float>(cells.y[i]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.numSteps = opts.maxIters;
    info.hpwl = getHpwl();
    info.hpwlInitial = hpwlInitial;
    info.overflow = overlap;
    info.mandatory = step == 0;
    d.onFrame(fx, fy, info);
}

void Kraftwerk2Placer::Impl::place(const Kraftwerk2Options &options) {
    ScopedTimer timer("kraftwerk2");
    opts = options;
    Cells &cells = d.cells;
    const std::size_t m = cells.size();
    if (m == 0 || !(d.die.getWidth() > 0.0) || !(d.die.getHeight() > 0.0)) {
        return;
    }
    double areaSum = 0.0;
    double widthSum = 0.0;
    double heightSum = 0.0;
    for (std::size_t i = 0; i < m; ++i) {
        areaSum += cells.width[i] * cells.height[i];
        widthSum += cells.width[i];
        heightSum += cells.height[i];
    }
    const double averageArea = areaSum / static_cast<double>(m);
    floorX = std::max(1e-9, widthSum / static_cast<double>(m));
    floorY = std::max(1e-9, heightSum / static_cast<double>(m));
    const double averageSize = (floorX + floorY) / 2.0;

    indexPins();
    placeInitially();
    hpwlInitial = getHpwl();
    writeFrame("Initial placement", 0, 1.0);

    const auto autoBins = static_cast<std::size_t>(std::sqrt(static_cast<double>(m)) / 2.0);
    potential = std::make_unique<Potential>(
        d.die,
        opts.binsPerAxis > 0 ? opts.binsPerAxis : std::clamp<std::size_t>(autoBins, 16, 512));
    const std::size_t n = potential->getBins();
    densityGrid = std::make_unique<BinGrid>(d.die, n, n);
    const double chipArea = d.die.getArea();
    const auto overlapBins = [&](double length) {
        return std::clamp<std::size_t>(static_cast<std::size_t>(length / averageSize), 1, 2048);
    };
    overlapGrid = std::make_unique<BinGrid>(d.die, overlapBins(d.die.getWidth()),
                                            overlapBins(d.die.getHeight()));
    std::vector<double> fixedInside(d.fixedBoxes.size(), 0.0);
    fixedCover.assign(overlapGrid->size(), 0.0);
    for (std::size_t f = 0; f < d.fixedBoxes.size(); ++f) {
        overlapGrid->rasterize(d.fixedBoxes[f], [&](std::size_t b, double area) {
            fixedCover[b] += area;
            fixedInside[f] += area;
        });
    }
    const double largeArea = opts.largeModuleArea * averageArea;
    const auto getModuleDensity = [&](double area) {
        if (area < largeArea || !(area > 0.0)) {
            return 1.0;
        }
        return std::sqrt(largeArea / area) * (1.0 - supplyDensity) + supplyDensity;
    };
    const auto getTotalDemand = [&] {
        double total = 0.0;
        for (std::size_t i = 0; i < m; ++i) {
            total += getModuleDensity(cells.width[i] * cells.height[i]) * cells.width[i] *
                     cells.height[i];
        }
        for (std::size_t f = 0; f < d.fixedBoxes.size(); ++f) {
            total += getModuleDensity(d.fixedBoxes[f].getArea()) * fixedInside[f];
        }
        return total;
    };
    if (opts.targetDensity > 0.0) {
        supplyDensity = std::min(1.0, opts.targetDensity);
    } else {
        supplyDensity = 1.0;
        for (int pass = 0; pass < 2; ++pass) {
            supplyDensity = std::min(1.0, getTotalDemand() / chipArea);
        }
    }
    moduleDensity.assign(m, 1.0);
    std::vector<double> moveWeight(m);
    for (std::size_t i = 0; i < m; ++i) {
        const double area = cells.width[i] * cells.height[i];
        moduleDensity[i] = getModuleDensity(area);
        moveWeight[i] = (area / averageArea) / static_cast<double>(m);
    }
    fixedDemand.assign(n * n, 0.0);
    for (const BBox &box : d.fixedBoxes) {
        const double density = getModuleDensity(box.getArea());
        densityGrid->rasterize(box, [&](std::size_t b, double area) {
            fixedDemand[b] += density * area;
        });
    }
    const double targetMovement = opts.targetMovement > 0.0 ? opts.targetMovement : averageSize;

    std::vector<Progress> progress;
    double weightScale = 1.0;
    double overlap = 1.0;
    std::size_t iteration = 0;
    bool converged = false;
    std::vector<double> dx(m);
    std::vector<double> dy(m);
    std::vector<double> towardX(m);
    std::vector<double> towardY(m);
    while (iteration < opts.maxIters) {
        const Density density = getDensity();
        overlap = density.overlap;
        if (overlap <= opts.targetOverlap) {
            converged = true;
            break;
        }
        ++iteration;
        potential->solve(density.charge);
        parallelFor(m, [&](std::size_t i) {
            const Point gradient = potential->getGradient(
                Point(cells.x[i] + cells.width[i] / 2.0, cells.y[i] + cells.height[i] / 2.0));
            towardX[i] = gradient.x;
            towardY[i] = gradient.y;
        });
        System sysX;
        System sysY;
        tbb::parallel_invoke(
            [&] {
                buildSystem(true, false, sysX);
            },
            [&] {
                buildSystem(false, false, sysY);
            });
        parallelFor(m, [&](std::size_t i) {
            sysX.diag[i] += moveWeight[i];
            sysY.diag[i] += moveWeight[i];
            sysX.rhs[i] = -moveWeight[i] * towardX[i];
            sysY.rhs[i] = -moveWeight[i] * towardY[i];
        });
        std::ranges::fill(dx, 0.0);
        std::ranges::fill(dy, 0.0);
        tbb::parallel_invoke(
            [&] {
                solve(sysX, dx, opts.cgTol, opts.cgMaxIter);
            },
            [&] {
                solve(sysY, dy, opts.cgTol, opts.cgMaxIter);
            });
        std::vector<double> oldX = cells.x;
        std::vector<double> oldY = cells.y;
        for (std::size_t i = 0; i < m; ++i) {
            cells.x[i] += dx[i];
            cells.y[i] += dy[i];
        }
        clampToDie();
        const double movement = getSum(m,
                                       [&](std::size_t i) {
                                           const double ex = cells.x[i] - oldX[i];
                                           const double ey = cells.y[i] - oldY[i];
                                           return std::sqrt(ex * ex + ey * ey);
                                       }) /
                                static_cast<double>(m);
        const double ratio = targetMovement / std::max(movement, 1e-12);
        const double kappa = 2.0 * ratio * ratio / (ratio * ratio + 1.0);
        for (double &w : moveWeight) {
            w *= kappa;
        }
        weightScale *= kappa;

        const bool report = opts.reportEvery > 0 && iteration % opts.reportEvery == 0;
        const bool trace = opts.traceEvery > 0 && iteration % opts.traceEvery == 0;
        if (report || trace) {
            const double hpwl = getHpwl();
            if (report) {
                progress.push_back(Progress{.iteration = iteration,
                                            .hpwl = hpwl,
                                            .overlap = overlap,
                                            .movement = movement,
                                            .weightScale = weightScale});
            }
            if (trace) {
                writeFrame(std::format("Kraftwerk2 iteration {}", iteration), iteration, overlap);
            }
        }
    }
    const double hpwlFinal = getHpwl();
    writeFrame("Global placement - Kraftwerk2", iteration, overlap);

    ktReportTable table("Kraftwerk2 convergence");
    table.setHeaders({"iteration", "HPWL", "overlap", "movement", "move weight x"});
    for (const Progress &row : progress) {
        table.addRow({std::format("{}", row.iteration), std::format("{:.6e}", row.hpwl),
                      std::format("{:.3f}", row.overlap), std::format("{:.3g}", row.movement),
                      std::format("{:.3g}", row.weightScale)});
    }
    table.emit();

    ktReportTable summary("Global placement (Kraftwerk2)");
    summary.setHeaders({"metric", "value"});
    summary.addRow({"iterations", std::format("{}", iteration)});
    summary.addRow({"stopped because", converged
                                           ? std::format("overlap below {}", opts.targetOverlap)
                                           : "iteration limit"});
    summary.addRow({"final overlap", std::format("{:.3f}", overlap)});
    summary.addRow({"potential grid", std::format("{0}x{0}", n)});
    summary.addRow({"target movement", std::format("{:.3g}", targetMovement)});
    summary.addRow({"HPWL after initial placement", std::format("{:.6e}", hpwlInitial)});
    summary.addRow({"HPWL final", std::format("{:.6e}", hpwlFinal)});
    summary.addRow({"seconds", std::format("{:.3f}", timer.getElapsedSeconds())});
    summary.emit();
    if (!converged) {
        ktlog.warning("Kraftwerk2 stopped at the iteration limit with overlap {:.3f}", overlap);
    }
}

Kraftwerk2Placer::Kraftwerk2Placer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

Kraftwerk2Placer::~Kraftwerk2Placer() = default;

void Kraftwerk2Placer::place(const Kraftwerk2Options &options) {
    pImpl->place(options);
}

}  // namespace ktplace::kraftwerk2
