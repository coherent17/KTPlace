// @file simpl_placer.cc
// SimPL global placement

#include "placer/simpl/simpl_placer.h"

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_invoke.h>
#include <oneapi/tbb/parallel_sort.h>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ktplace::simpl {

namespace {

constexpr std::uint32_t kNoVar = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t kGrain = 4096;

template <typename Body>
void parallelFor(std::size_t n, Body body) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, kGrain),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t i = range.begin(); i < range.end(); ++i) {
                              body(i);
                          }
                      });
}

template <typename Key>
void sortCells(std::vector<std::uint32_t> &cells, Key key) {
    tbb::parallel_sort(cells.begin(), cells.end(), [&](std::uint32_t a, std::uint32_t b) {
        const double ka = key(a);
        const double kb = key(b);
        return ka < kb || (ka == kb && a < b);
    });
}

template <typename Body>
double deterministicSum(std::size_t n, Body body) {
    const std::size_t chunks = (n + kGrain - 1) / kGrain;
    std::vector<double> partial(chunks, 0.0);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, chunks, 1),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t c = range.begin(); c < range.end(); ++c) {
                              const std::size_t end = std::min(n, (c + 1) * kGrain);
                              double sum = 0.0;
                              for (std::size_t i = c * kGrain; i < end; ++i) {
                                  sum += body(i);
                              }
                              partial[c] = sum;
                          }
                      });
    return std::accumulate(partial.begin(), partial.end(), 0.0);
}

double dot(const std::vector<double> &a, const std::vector<double> &b) {
    return deterministicSum(a.size(), [&](std::size_t i) {
        return a[i] * b[i];
    });
}

std::string frameStep(std::size_t n) {
    std::string s = std::to_string(n);
    return std::string(s.size() < 4 ? 4 - s.size() : 0, '0') + s;
}

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

struct Box {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
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

struct BinRect {
    std::size_t x0 = 0;
    std::size_t x1 = 0;
    std::size_t y0 = 0;
    std::size_t y1 = 0;

    [[nodiscard]] bool overlaps(const BinRect &o) const {
        return x0 <= o.x1 && o.x0 <= x1 && y0 <= o.y1 && o.y0 <= y1;
    }
    [[nodiscard]] BinRect merged(const BinRect &o) const {
        return {std::min(x0, o.x0), std::max(x1, o.x1), std::min(y0, o.y0), std::max(y1, o.y1)};
    }
};

struct AxisProfile {
    std::vector<double> edges;
    std::vector<double> prefix;

    [[nodiscard]] double total() const {
        return prefix.back();
    }
    [[nodiscard]] double before(double c) const {
        if (c <= edges.front()) {
            return 0.0;
        }
        if (c >= edges.back()) {
            return total();
        }
        const std::size_t k = static_cast<std::size_t>(
                                  std::upper_bound(edges.begin(), edges.end(), c) - edges.begin()) -
                              1;
        const double f = (c - edges[k]) / (edges[k + 1] - edges[k]);
        return prefix[k] + f * (prefix[k + 1] - prefix[k]);
    }
    [[nodiscard]] double between(double a, double b) const {
        return before(b) - before(a);
    }
    [[nodiscard]] double coordinateAt(double target) const {
        if (target <= 0.0) {
            return edges.front();
        }
        if (target >= total()) {
            return edges.back();
        }
        const std::size_t k = static_cast<std::size_t>(
            std::lower_bound(prefix.begin(), prefix.end(), target) - prefix.begin());
        const double step = prefix[k] - prefix[k - 1];
        const double f = step > 0.0 ? (target - prefix[k - 1]) / step : 1.0;
        return edges[k - 1] + f * (edges[k] - edges[k - 1]);
    }
};

struct BinGrid {
    double x0 = 0.0;
    double y0 = 0.0;
    double dx = 1.0;
    double dy = 1.0;
    std::size_t nx = 1;
    std::size_t ny = 1;
    std::vector<double> avail;
    double totalAvail = 0.0;

    [[nodiscard]] std::size_t size() const {
        return nx * ny;
    }
    [[nodiscard]] std::size_t at(std::size_t ix, std::size_t iy) const {
        return iy * nx + ix;
    }
    [[nodiscard]] std::size_t column(double x) const {
        return clampIndex((x - x0) / dx, nx);
    }
    [[nodiscard]] std::size_t row(double y) const {
        return clampIndex((y - y0) / dy, ny);
    }
    [[nodiscard]] std::size_t binOf(double x, double y) const {
        return at(column(x), row(y));
    }
    [[nodiscard]] double edge(bool vertical, std::size_t t) const {
        return vertical ? x0 + static_cast<double>(t) * dx : y0 + static_cast<double>(t) * dy;
    }
    [[nodiscard]] Box box(const BinRect &r) const {
        return {edge(true, r.x0), edge(false, r.y0), edge(true, r.x1 + 1), edge(false, r.y1 + 1)};
    }
    [[nodiscard]] double sum(const std::vector<double> &field, const BinRect &r) const {
        double total = 0.0;
        for (std::size_t iy = r.y0; iy <= r.y1; ++iy) {
            for (std::size_t ix = r.x0; ix <= r.x1; ++ix) {
                total += field[at(ix, iy)];
            }
        }
        return total;
    }
    [[nodiscard]] AxisProfile profile(const Box &b, bool vertical) const {
        const double lo = vertical ? b.x0 : b.y0;
        const double hi = vertical ? b.x1 : b.y1;
        const std::size_t first = vertical ? column(lo) : row(lo);
        const std::size_t last = vertical ? column(hi) : row(hi);
        const std::size_t crossFirst = vertical ? row(b.y0) : column(b.x0);
        const std::size_t crossLast = vertical ? row(b.y1) : column(b.x1);
        const double along = vertical ? dx : dy;
        const double across = vertical ? dy : dx;

        AxisProfile out;
        out.edges.push_back(lo);
        out.prefix.push_back(0.0);
        for (std::size_t t = first; t <= last; ++t) {
            const double a = std::max(lo, edge(vertical, t));
            const double z = std::min(hi, edge(vertical, t + 1));
            if (!(z > a)) {
                continue;
            }
            double room = 0.0;
            for (std::size_t u = crossFirst; u <= crossLast; ++u) {
                const double c0 = std::max(vertical ? b.y0 : b.x0, edge(!vertical, u));
                const double c1 = std::min(vertical ? b.y1 : b.x1, edge(!vertical, u + 1));
                if (c1 > c0) {
                    room += avail[vertical ? at(t, u) : at(u, t)] * (c1 - c0) / across;
                }
            }
            out.edges.push_back(z);
            out.prefix.push_back(out.prefix.back() + room * (z - a) / along);
        }
        if (out.edges.size() == 1) {
            out.edges.push_back(hi);
            out.prefix.push_back(0.0);
        }
        return out;
    }
    void deposit(const Box &b, double sign) {
        if (!(b.x1 > b.x0) || !(b.y1 > b.y0)) {
            return;
        }
        for (std::size_t iy = row(b.y0); iy <= row(b.y1); ++iy) {
            const double oy = std::min(b.y1, edge(false, iy + 1)) - std::max(b.y0, edge(false, iy));
            for (std::size_t ix = column(b.x0); ix <= column(b.x1); ++ix) {
                const double ox =
                    std::min(b.x1, edge(true, ix + 1)) - std::max(b.x0, edge(true, ix));
                if (ox > 0.0 && oy > 0.0) {
                    avail[at(ix, iy)] += sign * ox * oy;
                }
            }
        }
    }

    static std::size_t clampIndex(double f, std::size_t n) {
        return static_cast<std::size_t>(std::clamp(f, 0.0, static_cast<double>(n) - 1e-9));
    }
};

struct Block {
    Box box;
    std::size_t level = 1;
    bool vertical = true;
    std::vector<std::uint32_t> cells;
};

struct Stripe {
    double lo = 0.0;
    double hi = 0.0;
    double capacity = 0.0;
    double used = 0.0;
    std::vector<std::uint32_t> cells;
};

}  // namespace

class simplPlacer::Impl {
public:
    explicit Impl(const Design &d) : design(d), graph(d.graph) {}

    SimplResult run(const SimplOptions &params);

    std::vector<double> outX;
    std::vector<double> outY;

private:
    void collect();
    void buildGrid();
    void seedUniform(std::vector<double> &x, std::vector<double> &y) const;

    void buildSystems(const std::vector<double> &x, const std::vector<double> &y, double alpha,
                      const std::vector<double> &anchorX, const std::vector<double> &anchorY);
    void buildSystem(SparseSystem &sys, const std::vector<double> &pos, double lengthFloor,
                     double alpha, const std::vector<double> &anchor, bool vertical);
    void solve(std::vector<double> &x, std::vector<double> &y, const std::string &tag);
    [[nodiscard]] double hpwl(const std::vector<double> &x, const std::vector<double> &y) const;

    [[nodiscard]] double centre(const std::vector<double> &pos, std::uint32_t i,
                                bool vertical) const;
    [[nodiscard]] std::vector<double> occupancy(const std::vector<double> &x,
                                                const std::vector<double> &y) const;
    [[nodiscard]] double overflow(const std::vector<double> &occ) const;
    [[nodiscard]] double density(const std::vector<double> &occ, const BinRect &r) const;

    void lookAheadLegalize(std::vector<double> &x, std::vector<double> &y) const;
    [[nodiscard]] std::vector<BinRect> overfilledRegions(const std::vector<double> &occ) const;
    [[nodiscard]] BinRect grown(BinRect r, const std::vector<double> &occ) const;
    void legalizeBlock(Block &block, std::vector<double> &x, std::vector<double> &y) const;
    [[nodiscard]] std::optional<std::pair<Block, Block>> split(Block &block, std::vector<double> &x,
                                                               std::vector<double> &y) const;
    void nonlinearScale(const std::vector<std::uint32_t> &cells, const Box &box, bool vertical,
                        double cutline, bool belowCut, std::vector<double> &pos) const;
    [[nodiscard]] std::vector<Stripe> stripes(const Box &box, bool vertical, double cellArea) const;
    void clampToDie(std::vector<double> &x, std::vector<double> &y) const;

    void enforceFences(std::vector<double> &x, std::vector<double> &y);

    void writeFrame(const std::string &series, const std::vector<double> &x,
                    const std::vector<double> &y, const std::string &note, std::size_t step,
                    std::size_t total);
    void writeDensityMap(const std::string &series, std::size_t step, const std::vector<double> &x,
                         const std::vector<double> &y, const std::string &note) const;
    void writeBounds(const std::vector<std::pair<double, double>> &bounds) const;

    const Design &design;
    const Graph &graph;
    const Fences *fences = nullptr;

    SimplOptions par;
    SimplResult res;

    std::size_t numMovable = 0;
    std::vector<std::uint32_t> movVertex;
    std::vector<std::uint32_t> varOfVertex;
    std::vector<double> width;
    std::vector<double> height;
    std::vector<double> area;
    std::vector<int> regionOf;
    std::vector<double> fixedX;
    std::vector<double> fixedY;
    std::vector<Box> obstacles;
    std::vector<NetPins> nets;
    std::vector<std::size_t> pinStart;
    std::vector<PinRef> pinsOfCell;

    SparseSystem sysX;
    SparseSystem sysY;
    double floorX = 1.0;
    double floorY = 1.0;

    BinGrid grid;
    std::array<double, 4> die{};
    double g = 1.0;
};

void simplPlacer::Impl::collect() {
    const std::size_t nv = graph.getNumCells();
    varOfVertex.assign(nv, kNoVar);
    fixedX.assign(nv, 0.0);
    fixedY.assign(nv, 0.0);
    for (std::size_t v = 0; v < nv; ++v) {
        const Cell &cell = graph.getCell(v);
        fixedX[v] = cell.x;
        fixedY[v] = cell.y;
        if (cell.isFixed || cell.isTerminal) {
            if (cell.width > 0.0 && cell.height > 0.0) {
                obstacles.push_back({cell.x, cell.y, cell.x + cell.width, cell.y + cell.height});
            }
            ++res.numFixed;
            continue;
        }
        varOfVertex[v] = static_cast<std::uint32_t>(numMovable++);
        movVertex.push_back(static_cast<std::uint32_t>(v));
        width.push_back(cell.width);
        height.push_back(cell.height);
        area.push_back(cell.width * cell.height);
        regionOf.push_back(cell.regionId);
    }

    nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < nets.size(); ++n) {
        const std::vector<std::size_t> &pins = graph.getNetPins(n);
        std::vector<std::size_t> order(pins.begin(), pins.end());
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return graph.getPin(a).cellId < graph.getPin(b).cellId;
        });
        NetPins &net = nets[n];
        net.weight = graph.getNet(n).weight;
        for (const std::size_t pinId : order) {
            const Pin &pin = graph.getPin(pinId);
            if (!net.cells.empty() && net.cells.back() == pin.cellId) {
                continue;
            }
            net.cells.push_back(static_cast<std::uint32_t>(pin.cellId));
            net.offX.push_back(pin.offsetX);
            net.offY.push_back(pin.offsetY);
        }
    }

    pinStart.assign(numMovable + 1, 0);
    for (const NetPins &net : nets) {
        for (const std::uint32_t c : net.cells) {
            if (varOfVertex[c] != kNoVar) {
                ++pinStart[varOfVertex[c] + 1];
            }
        }
    }
    std::partial_sum(pinStart.begin(), pinStart.end(), pinStart.begin());
    pinsOfCell.resize(pinStart.back());
    std::vector<std::size_t> cursor(pinStart.begin(), pinStart.end() - 1);
    for (std::uint32_t n = 0; n < nets.size(); ++n) {
        for (std::uint32_t q = 0; q < nets[n].cells.size(); ++q) {
            const std::uint32_t v = varOfVertex[nets[n].cells[q]];
            if (v != kNoVar) {
                pinsOfCell[cursor[v]++] = {n, q};
            }
        }
    }

    res.numMovable = numMovable;
    res.nets = nets.size();
    if (numMovable > 0) {
        floorX = std::accumulate(width.begin(), width.end(), 0.0) / static_cast<double>(numMovable);
        floorY =
            std::accumulate(height.begin(), height.end(), 0.0) / static_cast<double>(numMovable);
    }
}

void simplPlacer::Impl::buildGrid() {
    die = design.die;
    const std::size_t perAxis =
        par.binsPerAxis > 0
            ? par.binsPerAxis
            : static_cast<std::size_t>(std::clamp(
                  std::sqrt(static_cast<double>(std::max<std::size_t>(numMovable, 1)) / 51.0), 16.0,
                  256.0));
    grid.x0 = die[0];
    grid.y0 = die[1];
    grid.nx = perAxis;
    grid.ny = perAxis;
    grid.dx = std::max(die[2] - die[0], 1e-9) / static_cast<double>(perAxis);
    grid.dy = std::max(die[3] - die[1], 1e-9) / static_cast<double>(perAxis);
    grid.avail.assign(grid.size(), 0.0);

    double smallestSite = std::numeric_limits<double>::max();
    for (const RowInfo &row : design.rows) {
        if (!(row.height > 0.0) || !(row.pitch() > 0.0)) {
            continue;
        }
        smallestSite = std::min(smallestSite, row.pitch() * row.height);
        for (const Subrow &sub : row.subrows) {
            grid.deposit({sub.xlo, row.coordinate, sub.xhi, row.coordinate + row.height}, 1.0);
        }
    }
    for (const Box &obstacle : obstacles) {
        grid.deposit(obstacle, -1.0);
    }
    const double sliver =
        smallestSite < std::numeric_limits<double>::max() ? 1e-3 * smallestSite : 0.0;
    for (double &a : grid.avail) {
        a = a < sliver ? 0.0 : a;
    }
    grid.totalAvail = std::accumulate(grid.avail.begin(), grid.avail.end(), 0.0);
    res.binsX = grid.nx;
    res.binsY = grid.ny;
}

void simplPlacer::Impl::seedUniform(std::vector<double> &x, std::vector<double> &y) const {
    std::mt19937_64 rng(par.seed);
    const auto unit = [&rng] {
        return static_cast<double>(rng() >> 11) * 0x1.0p-53;
    };
    x.resize(numMovable);
    y.resize(numMovable);
    for (std::size_t i = 0; i < numMovable; ++i) {
        x[i] = die[0] + unit() * std::max(die[2] - die[0] - width[i], 0.0);
        y[i] = die[1] + unit() * std::max(die[3] - die[1] - height[i], 0.0);
    }
}

void simplPlacer::Impl::buildSystems(const std::vector<double> &x, const std::vector<double> &y,
                                     double alpha, const std::vector<double> &anchorX,
                                     const std::vector<double> &anchorY) {
    std::vector<double> px = fixedX;
    std::vector<double> py = fixedY;
    for (std::size_t i = 0; i < numMovable; ++i) {
        px[movVertex[i]] = x[i];
        py[movVertex[i]] = y[i];
    }
    tbb::parallel_invoke(
        [&] {
            buildSystem(sysX, px, floorX, alpha, anchorX, true);
        },
        [&] {
            buildSystem(sysY, py, floorY, alpha, anchorY, false);
        });
}

void simplPlacer::Impl::buildSystem(SparseSystem &sys, const std::vector<double> &pos,
                                    double lengthFloor, double alpha,
                                    const std::vector<double> &anchor, bool vertical) {
    const auto pinAt = [&](const NetPins &net, std::size_t q) {
        return pos[net.cells[q]] + (vertical ? net.offX[q] : net.offY[q]);
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extremes(nets.size());
    parallelFor(nets.size(), [&](std::size_t n) {
        const NetPins &net = nets[n];
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        for (std::uint32_t q = 1; q < net.cells.size(); ++q) {
            lo = pinAt(net, q) < pinAt(net, lo) ? q : lo;
            hi = pinAt(net, q) > pinAt(net, hi) ? q : hi;
        }
        extremes[n] = {lo, hi};
    });

    const auto forEachEdge = [&](std::size_t i, auto &&visit) {
        for (std::size_t e = pinStart[i]; e < pinStart[i + 1]; ++e) {
            const auto [n, q] = pinsOfCell[e];
            const NetPins &net = nets[n];
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

    sys.rowPtr.assign(numMovable + 1, 0);
    parallelFor(numMovable, [&](std::size_t i) {
        std::size_t count = 0;
        forEachEdge(i, [&](std::uint32_t cell, double, double, double) {
            count += varOfVertex[cell] != kNoVar ? 1 : 0;
        });
        sys.rowPtr[i + 1] = count;
    });
    std::partial_sum(sys.rowPtr.begin(), sys.rowPtr.end(), sys.rowPtr.begin());
    sys.col.resize(sys.rowPtr.back());
    sys.val.resize(sys.rowPtr.back());
    sys.diag.assign(numMovable, 0.0);
    sys.rhs.assign(numMovable, 0.0);

    parallelFor(numMovable, [&](std::size_t i) {
        std::size_t slot = sys.rowPtr[i];
        double diag = 0.0;
        double rhs = 0.0;
        forEachEdge(i, [&](std::uint32_t cell, double pin, double ownOffset, double w) {
            const std::uint32_t var = varOfVertex[cell];
            diag += w;
            if (var != kNoVar) {
                sys.col[slot] = var;
                sys.val[slot] = -w;
                ++slot;
                rhs += w * ((pin - pos[cell]) - ownOffset);
            } else {
                rhs += w * (pin - ownOffset);
            }
        });
        if (alpha > 0.0) {
            const double w = alpha / std::max(std::abs(pos[movVertex[i]] - anchor[i]), lengthFloor);
            diag += w;
            rhs += w * anchor[i];
        }
        sys.diag[i] = std::max(diag, 1e-12);
        sys.rhs[i] = rhs;
    });
}

void simplPlacer::Impl::solve(std::vector<double> &x, std::vector<double> &y,
                              const std::string &tag) {
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
        return a.active && a.resid > par.cgTol;
    };
    for (std::size_t it = 0; it < par.cgMaxIter && (live(ax) || live(ay)); ++it) {
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
        const bool last = !live(ax) && !live(ay);
        if (par.cgEvery > 0 && ((it + 1) % par.cgEvery == 0 || last)) {
            writeFrame("cg_" + tag, x, y,
                       fmt::format("CG {} iterate {}, residual x {:.3e} / y {:.3e}", tag, it + 1,
                                   ax.resid, ay.resid),
                       it + 1, par.cgMaxIter);
        }
    }
    ktlog.trace("  cg {}: residual x {:.3e} y {:.3e}", tag, ax.resid, ay.resid);
}

double simplPlacer::Impl::hpwl(const std::vector<double> &x, const std::vector<double> &y) const {
    std::vector<double> px = fixedX;
    std::vector<double> py = fixedY;
    for (std::size_t i = 0; i < numMovable; ++i) {
        px[movVertex[i]] = x[i];
        py[movVertex[i]] = y[i];
    }
    return deterministicSum(nets.size(), [&](std::size_t n) {
        const NetPins &net = nets[n];
        if (net.cells.size() < 2) {
            return 0.0;
        }
        double x0 = std::numeric_limits<double>::max();
        double x1 = -x0;
        double y0 = x0;
        double y1 = -x0;
        for (std::size_t q = 0; q < net.cells.size(); ++q) {
            const double pxq = px[net.cells[q]] + net.offX[q];
            const double pyq = py[net.cells[q]] + net.offY[q];
            x0 = std::min(x0, pxq);
            x1 = std::max(x1, pxq);
            y0 = std::min(y0, pyq);
            y1 = std::max(y1, pyq);
        }
        return (x1 - x0) + (y1 - y0);
    });
}

double simplPlacer::Impl::centre(const std::vector<double> &pos, std::uint32_t i,
                                 bool vertical) const {
    return pos[i] + 0.5 * (vertical ? width[i] : height[i]);
}

std::vector<double> simplPlacer::Impl::occupancy(const std::vector<double> &x,
                                                 const std::vector<double> &y) const {
    std::vector<double> occ(grid.size(), 0.0);
    for (std::uint32_t i = 0; i < numMovable; ++i) {
        occ[grid.binOf(centre(x, i, true), centre(y, i, false))] += area[i];
    }
    return occ;
}

double simplPlacer::Impl::overflow(const std::vector<double> &occ) const {
    double excess = 0.0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        excess += std::max(occ[k] - g * grid.avail[k], 0.0);
    }
    return grid.totalAvail > 0.0 ? excess / grid.totalAvail : 0.0;
}

double simplPlacer::Impl::density(const std::vector<double> &occ, const BinRect &r) const {
    const double cells = grid.sum(occ, r);
    const double room = grid.sum(grid.avail, r);
    if (room > 0.0) {
        return cells / room;
    }
    return cells > 0.0 ? std::numeric_limits<double>::infinity() : 0.0;
}

void simplPlacer::Impl::lookAheadLegalize(std::vector<double> &x, std::vector<double> &y) const {
    std::vector<std::vector<std::uint32_t>> members(grid.size());
    std::vector<double> occ(grid.size(), 0.0);
    for (std::uint32_t i = 0; i < numMovable; ++i) {
        const std::size_t k = grid.binOf(centre(x, i, true), centre(y, i, false));
        members[k].push_back(i);
        occ[k] += area[i];
    }

    const std::vector<BinRect> regions = overfilledRegions(occ);
    tbb::parallel_for(std::size_t{0}, regions.size(), [&](std::size_t r) {
        const BinRect &bins = regions[r];
        Block top{grid.box(bins), 1, true, {}};
        for (std::size_t iy = bins.y0; iy <= bins.y1; ++iy) {
            for (std::size_t ix = bins.x0; ix <= bins.x1; ++ix) {
                const std::vector<std::uint32_t> &in = members[grid.at(ix, iy)];
                top.cells.insert(top.cells.end(), in.begin(), in.end());
            }
        }
        legalizeBlock(top, x, y);
    });
    clampToDie(x, y);
}

void simplPlacer::Impl::legalizeBlock(Block &block, std::vector<double> &x,
                                      std::vector<double> &y) const {
    auto halves = split(block, x, y);
    if (!halves) {
        return;
    }
    tbb::parallel_invoke(
        [&] {
            legalizeBlock(halves->first, x, y);
        },
        [&] {
            legalizeBlock(halves->second, x, y);
        });
}

std::vector<BinRect> simplPlacer::Impl::overfilledRegions(const std::vector<double> &occ) const {
    std::vector<char> over(grid.size(), 0);
    for (std::size_t k = 0; k < grid.size(); ++k) {
        over[k] = occ[k] > g * grid.avail[k] ? 1 : 0;
    }

    std::vector<BinRect> regions;
    std::vector<std::size_t> frontier;
    for (std::size_t seed = 0; seed < grid.size(); ++seed) {
        if (!over[seed]) {
            continue;
        }
        over[seed] = 0;
        frontier.assign(1, seed);
        BinRect r{seed % grid.nx, seed % grid.nx, seed / grid.nx, seed / grid.nx};
        while (!frontier.empty()) {
            const std::size_t k = frontier.back();
            frontier.pop_back();
            const std::size_t ix = k % grid.nx;
            const std::size_t iy = k / grid.nx;
            r = r.merged({ix, ix, iy, iy});
            const std::array<std::pair<long, long>, 4> steps{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
            for (const auto &[sx, sy] : steps) {
                const long nx = static_cast<long>(ix) + sx;
                const long ny = static_cast<long>(iy) + sy;
                if (nx < 0 || ny < 0 || nx >= static_cast<long>(grid.nx) ||
                    ny >= static_cast<long>(grid.ny)) {
                    continue;
                }
                const std::size_t kk =
                    grid.at(static_cast<std::size_t>(nx), static_cast<std::size_t>(ny));
                if (over[kk]) {
                    over[kk] = 0;
                    frontier.push_back(kk);
                }
            }
        }
        regions.push_back(grown(r, occ));
    }

    for (bool mergedAny = true; mergedAny;) {
        mergedAny = false;
        for (std::size_t a = 0; a < regions.size(); ++a) {
            for (std::size_t b = a + 1; b < regions.size();) {
                if (regions[a].overlaps(regions[b])) {
                    regions[a] = grown(regions[a].merged(regions[b]), occ);
                    regions.erase(regions.begin() + static_cast<long>(b));
                    mergedAny = true;
                } else {
                    ++b;
                }
            }
        }
    }
    return regions;
}

BinRect simplPlacer::Impl::grown(BinRect r, const std::vector<double> &occ) const {
    while (density(occ, r) > g) {
        const BinRect before = r;
        r.x0 -= r.x0 > 0 ? 1 : 0;
        r.y0 -= r.y0 > 0 ? 1 : 0;
        r.x1 += r.x1 + 1 < grid.nx ? 1 : 0;
        r.y1 += r.y1 + 1 < grid.ny ? 1 : 0;
        if (r.x0 == before.x0 && r.x1 == before.x1 && r.y0 == before.y0 && r.y1 == before.y1) {
            break;
        }
    }
    return r;
}

std::optional<std::pair<Block, Block>> simplPlacer::Impl::split(Block &region,
                                                                std::vector<double> &x,
                                                                std::vector<double> &y) const {
    if (region.level >= par.maxLevel || region.cells.size() <= par.minCellsToSplit) {
        return std::nullopt;
    }
    const bool vertical = region.vertical;
    std::vector<double> &pos = vertical ? x : y;

    std::vector<std::uint32_t> &cells = region.cells;
    sortCells(cells, [&](std::uint32_t i) {
        return centre(pos, i, vertical);
    });
    const double cellArea =
        std::accumulate(cells.begin(), cells.end(), 0.0, [&](double s, std::uint32_t i) {
            return s + area[i];
        });
    std::size_t m = 0;
    double lowArea = 0.0;
    while (m + 1 < cells.size() && (m == 0 || lowArea < 0.5 * cellArea)) {
        lowArea += area[cells[m++]];
    }
    std::vector<std::uint32_t> low(cells.begin(), cells.begin() + static_cast<long>(m));
    std::vector<std::uint32_t> high(cells.begin() + static_cast<long>(m), cells.end());

    const Box &box = region.box;
    const double lo = vertical ? box.x0 : box.y0;
    const double hi = vertical ? box.x1 : box.y1;
    const AxisProfile room = grid.profile(box, vertical);
    const double share = cellArea > 0.0 ? lowArea / cellArea : 0.5;
    const double cutline =
        room.total() > 0.0 ? room.coordinateAt(share * room.total()) : lo + share * (hi - lo);

    Box lowBox = box;
    Box highBox = box;
    (vertical ? lowBox.x1 : lowBox.y1) = cutline;
    (vertical ? highBox.x0 : highBox.y0) = cutline;

    nonlinearScale(low, lowBox, vertical, cutline, true, pos);
    nonlinearScale(high, highBox, vertical, cutline, false, pos);

    return std::pair{Block{lowBox, region.level + 1, !vertical, std::move(low)},
                     Block{highBox, region.level + 1, !vertical, std::move(high)}};
}

std::vector<Stripe> simplPlacer::Impl::stripes(const Box &box, bool vertical,
                                               double cellArea) const {
    const double lo = vertical ? box.x0 : box.y0;
    const double hi = vertical ? box.x1 : box.y1;
    const AxisProfile room = grid.profile(box, vertical);

    std::vector<double> cuts{lo, hi};
    for (const Box &o : obstacles) {
        if (o.x1 <= box.x0 || o.x0 >= box.x1 || o.y1 <= box.y0 || o.y0 >= box.y1) {
            continue;
        }
        for (const double edge : {vertical ? o.x0 : o.y0, vertical ? o.x1 : o.y1}) {
            if (edge > lo && edge < hi) {
                cuts.push_back(edge);
            }
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

    const double limit = par.stripeAreaFraction * room.total();
    const double fill = room.total() > 0.0 ? std::min(g, cellArea / room.total()) : g;
    for (std::size_t s = 0; s + 1 < cuts.size();) {
        const double a = cuts[s];
        const double b = cuts[s + 1];
        const double here = room.between(a, b);
        const double mid = room.coordinateAt(room.before(a) + 0.5 * here);
        if (here > limit && mid > a && mid < b) {
            cuts.insert(cuts.begin() + static_cast<long>(s) + 1, mid);
        } else {
            ++s;
        }
    }

    std::vector<Stripe> out;
    for (std::size_t s = 0; s + 1 < cuts.size(); ++s) {
        out.push_back({cuts[s], cuts[s + 1], fill * room.between(cuts[s], cuts[s + 1]), 0.0, {}});
    }
    return out;
}

void simplPlacer::Impl::nonlinearScale(const std::vector<std::uint32_t> &cells, const Box &box,
                                       bool vertical, double cutline, bool belowCut,
                                       std::vector<double> &pos) const {
    if (cells.empty()) {
        return;
    }
    const double cellArea =
        std::accumulate(cells.begin(), cells.end(), 0.0, [&](double sum, std::uint32_t i) {
            return sum + area[i];
        });
    std::vector<Stripe> strips = stripes(box, vertical, cellArea);
    const auto fromCut = [&](double c) {
        return belowCut ? cutline - c : c - cutline;
    };
    std::sort(strips.begin(), strips.end(), [&](const Stripe &a, const Stripe &b) {
        return fromCut(0.5 * (a.lo + a.hi)) > fromCut(0.5 * (b.lo + b.hi));
    });

    std::vector<std::uint32_t> order = cells;
    sortCells(order, [&](std::uint32_t i) {
        return -fromCut(centre(pos, i, vertical));
    });
    std::size_t s = 0;
    for (const std::uint32_t i : order) {
        while (s + 1 < strips.size() && strips[s].used >= strips[s].capacity) {
            ++s;
        }
        strips[s].cells.push_back(i);
        strips[s].used += area[i];
    }

    for (const Stripe &strip : strips) {
        if (strip.cells.empty()) {
            continue;
        }
        double lo = std::numeric_limits<double>::max();
        double hi = -lo;
        for (const std::uint32_t i : strip.cells) {
            lo = std::min(lo, centre(pos, i, vertical));
            hi = std::max(hi, centre(pos, i, vertical));
        }
        for (const std::uint32_t i : strip.cells) {
            const double t = hi > lo ? (centre(pos, i, vertical) - lo) / (hi - lo) : 0.5;
            const double size = vertical ? width[i] : height[i];
            const double c = strip.lo + t * (strip.hi - strip.lo);
            pos[i] = std::clamp(c - 0.5 * size, strip.lo, std::max(strip.lo, strip.hi - size));
        }
    }
}

void simplPlacer::Impl::clampToDie(std::vector<double> &x, std::vector<double> &y) const {
    for (std::size_t i = 0; i < numMovable; ++i) {
        x[i] = std::clamp(x[i], die[0], std::max(die[0], die[2] - width[i]));
        y[i] = std::clamp(y[i], die[1], std::max(die[1], die[3] - height[i]));
    }
}

void simplPlacer::Impl::enforceFences(std::vector<double> &x, std::vector<double> &y) {
    if (fences == nullptr || fences->numRegions() == 0) {
        return;
    }
    for (std::size_t i = 0; i < numMovable; ++i) {
        if (regionOf[i] != Fences::kNoRegion) {
            res.fenceClamps += fences->clampToRegion(regionOf[i], x[i], y[i]) ? 1 : 0;
        } else {
            res.fencePushes +=
                fences->pushOutOfRegions(x[i], y[i], die[0], die[1], die[2], die[3]) ? 1 : 0;
        }
    }
}

void simplPlacer::Impl::writeFrame(const std::string &series, const std::vector<double> &x,
                                   const std::vector<double> &y, const std::string &note,
                                   std::size_t step, std::size_t total) {
    if (!design.onFrame) {
        return;
    }
    std::vector<float> fx(fixedX.begin(), fixedX.end());
    std::vector<float> fy(fixedY.begin(), fixedY.end());
    for (std::size_t i = 0; i < numMovable; ++i) {
        fx[movVertex[i]] = static_cast<float>(x[i]);
        fy[movVertex[i]] = static_cast<float>(y[i]);
    }
    FrameInfo info;
    info.note = note;
    info.series = series;
    info.step = step;
    info.numSteps = total;
    info.hpwl = hpwl(x, y);
    info.hpwlInitial = res.hpwlSeed;
    info.overflow = overflow(occupancy(x, y));
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

void simplPlacer::Impl::writeDensityMap(const std::string &series, std::size_t step,
                                        const std::vector<double> &x, const std::vector<double> &y,
                                        const std::string &note) const {
    if (!par.densityMaps || !design.onSvg) {
        return;
    }
    const std::vector<double> occ = occupancy(x, y);
    constexpr double kW = 900.0;
    constexpr double kH = 620.0;
    constexpr double kPad = 52.0;
    const double sx = (kW - 2 * kPad) / std::max(die[2] - die[0], 1e-9);
    const double sy = (kH - 2 * kPad) / std::max(die[3] - die[1], 1e-9);
    const auto colour = [](double u) {
        if (u > 1.0) {
            const int fade = static_cast<int>(90.0 * (1.0 - std::min((u - 1.0) / 2.0, 1.0)));
            return fmt::format("rgb(255,{},{})", fade, fade);
        }
        return fmt::format("rgb({},{},{})", static_cast<int>(40.0 + 90.0 * u),
                           static_cast<int>(90.0 + 150.0 * u), static_cast<int>(190.0 * (1.0 - u)));
    };
    const auto rect = [&](std::ostringstream &svg, const Box &b, const std::string &fill) {
        svg << "<rect x=\"" << kPad + (b.x0 - die[0]) * sx << "\" y=\""
            << kH - kPad - (b.y1 - die[1]) * sy << "\" width=\"" << (b.x1 - b.x0) * sx
            << "\" height=\"" << (b.y1 - b.y0) * sy << "\" fill=\"" << fill << "\"/>\n";
    };

    std::ostringstream svg;
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << kW << "\" height=\"" << kH
        << "\">\n<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    for (std::size_t iy = 0; iy < grid.ny; ++iy) {
        for (std::size_t ix = 0; ix < grid.nx; ++ix) {
            const std::size_t k = grid.at(ix, iy);
            if (grid.avail[k] > 0.0) {
                rect(svg, grid.box({ix, ix, iy, iy}), colour(occ[k] / grid.avail[k]));
            }
        }
    }
    for (const Box &o : obstacles) {
        rect(svg, o, "#bdbdbd");
    }
    svg << "<text x=\"" << kPad
        << "\" y=\"24\" fill=\"#e0e0e0\" font-family=\"monospace\" font-size=\"14\">"
        << "bin density " << note << "  ovf " << fmt::format("{:.4f}", overflow(occ))
        << "</text>\n</svg>\n";
    design.onSvg(series, std::to_string(step), svg.str());
}

void simplPlacer::Impl::writeBounds(const std::vector<std::pair<double, double>> &bounds) const {
    if (design.artifactDir.empty()) {
        return;
    }
    std::ofstream csv(design.artifactDir + "/simpl_bounds.csv");
    csv << "iter,hpwl_upper,hpwl_lower,gap\n";
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        csv << i << ',' << bounds[i].first << ',' << bounds[i].second << ','
            << bounds[i].first - bounds[i].second << '\n';
    }

    double top = 1e-12;
    for (const auto &[upper, lower] : bounds) {
        top = std::max({top, upper, lower});
    }
    constexpr double kW = 780.0;
    constexpr double kH = 340.0;
    constexpr double kPad = 60.0;
    const auto px = [&](std::size_t i) {
        return kPad + (kW - 2 * kPad) *
                          (bounds.size() < 2
                               ? 0.0
                               : static_cast<double>(i) / static_cast<double>(bounds.size() - 1));
    };
    const auto py = [&](double v) {
        return kH - kPad - (kH - 2 * kPad) * std::clamp(v / top, 0.0, 1.0);
    };
    const auto line = [&](bool upper, const char *colour) {
        std::ostringstream s;
        s << "<polyline fill='none' stroke='" << colour << "' stroke-width='2' points='";
        for (std::size_t i = 0; i < bounds.size(); ++i) {
            s << px(i) << ',' << py(upper ? bounds[i].first : bounds[i].second) << ' ';
        }
        s << "'/>\n";
        return s.str();
    };
    std::ofstream svg(design.artifactDir + "/simpl_bounds.svg");
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << kW << "' height='" << kH << "'>\n"
        << "<rect width='100%' height='100%' fill='white'/>\n"
        << "<text x='" << kPad << "' y='24' font-family='monospace' font-size='14'>"
        << "SimPL: HPWL of the lower and upper bounds</text>\n"
        << "<line x1='" << kPad << "' y1='" << kH - kPad << "' x2='" << kW - kPad << "' y2='"
        << kH - kPad << "' stroke='#333'/>\n"
        << "<line x1='" << kPad << "' y1='" << kPad << "' x2='" << kPad << "' y2='" << kH - kPad
        << "' stroke='#333'/>\n"
        << line(false, "#2471a3") << line(true, "#c0392b") << "<text x='" << kW - kPad - 130
        << "' y='" << kPad + 12 << "' font-family='monospace' font-size='12' fill='#2471a3'>"
        << "lower bound</text>\n<text x='" << kW - kPad - 130 << "' y='" << kPad + 28
        << "' font-family='monospace' font-size='12' fill='#c0392b'>upper bound</text>\n</svg>\n";
}

SimplResult simplPlacer::Impl::run(const SimplOptions &params) {
    par = params;
    g = std::clamp(par.densityLimit, 0.05, 1.0);
    fences = params.fences ? &design.fences : nullptr;
    res.fenceRegions = fences != nullptr ? fences->numRegions() : 0;

    {
        ScopedTimer setup("simpl-setup");
        collect();
        buildGrid();
    }

    outX = fixedX;
    outY = fixedY;
    if (numMovable == 0) {
        return res;
    }

    std::vector<double> lower;
    std::vector<double> lowerY;
    seedUniform(lower, lowerY);
    res.hpwlSeed = hpwl(lower, lowerY);
    ktlog.echo(
        "SimPL: {} movable cells, {} fixed, {} nets; grid {}x{}, available area {:.4g}, "
        "density limit g = {:.2f}; uniform seed {} (HPWL {:.6e})",
        numMovable, res.numFixed, res.nets, grid.nx, grid.ny, grid.totalAvail, g, par.seed,
        res.hpwlSeed);
    writeFrame("LSS_init_seed", lower, lowerY, "initial placement: uniform seed", 0,
               par.initMaxIters);

    const std::vector<double> noAnchors;
    res.hpwlInit = res.hpwlSeed;
    std::size_t accepted = 0;
    std::size_t framed = 0;
    for (std::size_t it = 0; it < par.initMaxIters; ++it) {
        ScopedTimer timer("simpl-init");
        std::vector<double> x = lower;
        std::vector<double> y = lowerY;
        buildSystems(x, y, 0.0, noAnchors, noAnchors);
        solve(x, y, "init" + frameStep(it));
        res.initIters = it + 1;
        const double h = hpwl(x, y);
        ktlog.trace("init iter {:2d}: hpwl {:.6e}", it, h);
        if (!(h < res.hpwlInit)) {
            break;
        }
        lower = std::move(x);
        lowerY = std::move(y);
        res.hpwlInit = h;
        accepted = it + 1;
        if ((accepted & (accepted - 1)) == 0) {
            framed = accepted;
            writeFrame("LSS_init", lower, lowerY,
                       fmt::format("initial placement round {}", accepted), accepted,
                       par.initMaxIters);
        }
    }
    if (accepted > 0 && framed != accepted) {
        writeFrame("LSS_init", lower, lowerY, fmt::format("initial placement round {}", accepted),
                   accepted, par.initMaxIters);
    }
    writeDensityMap("density_init", res.initIters, lower, lowerY, "initial placement");

    std::vector<double> upper;
    std::vector<double> upperY;
    std::vector<std::pair<double, double>> bounds;
    double gapReference = 0.0;
    double previousUpper = std::numeric_limits<double>::max();
    for (std::size_t it = 0; it < par.maxIters; ++it) {
        res.globalIters = it + 1;
        {
            ScopedTimer timer("simpl-legalize");
            upper = lower;
            upperY = lowerY;
            lookAheadLegalize(upper, upperY);
            enforceFences(upper, upperY);
            res.spreadSeconds += timer.elapsedSeconds();
        }

        const double upperHpwl = hpwl(upper, upperY);
        const double lowerHpwl = hpwl(lower, lowerY);
        const double gap = upperHpwl - lowerHpwl;
        bounds.emplace_back(upperHpwl, lowerHpwl);
        ktlog.trace("iter {:3d}: lower {:.6e} upper {:.6e} gap {:.4e} overflow {:.4f} -> {:.4f}",
                    it, lowerHpwl, upperHpwl, gap, overflow(occupancy(lower, lowerY)),
                    overflow(occupancy(upper, upperY)));
        if (par.traceEvery > 0 && it % par.traceEvery == 0) {
            writeFrame("LSS", lower, lowerY,
                       fmt::format("LSS (linear system solve) - iteration {}", it), it,
                       par.maxIters);
            writeFrame("LAL", upper, upperY,
                       fmt::format("LAL (look-ahead legalized) - iteration {}", it), it,
                       par.maxIters);
            writeDensityMap("density_LSS", it, lower, lowerY, fmt::format("LSS iteration {}", it));
            writeDensityMap("density_LAL", it, upper, upperY, fmt::format("LAL iteration {}", it));
        }

        const std::size_t iteration = it + 1;
        if (iteration == par.gapReferenceIter) {
            gapReference = gap;
        }
        const bool upperStalled = upperHpwl >= previousUpper;
        previousUpper = upperHpwl;
        if (iteration > par.gapReferenceIter && gapReference > 0.0 &&
            (gap < par.gapTightFrac * gapReference ||
             (gap <= par.gapRelaxedFrac * gapReference && upperStalled))) {
            ktlog.trace("converged at iteration {}: gap {:.4e}, reference {:.4e}", it, gap,
                        gapReference);
            break;
        }

        const double alpha = par.alphaBase * (1.0 + static_cast<double>(it));
        {
            ScopedTimer timer("simpl-build");
            buildSystems(lower, lowerY, alpha, upper, upperY);
            res.buildSeconds += timer.elapsedSeconds();
        }
        {
            ScopedTimer timer("simpl-solve");
            solve(lower, lowerY, "g" + frameStep(it));
            res.solveSeconds += timer.elapsedSeconds();
        }
    }

    if (upper.empty()) {
        upper = lower;
        upperY = lowerY;
        enforceFences(upper, upperY);
    }

    res.hpwlFinal = hpwl(upper, upperY);
    res.hpwlLower = hpwl(lower, lowerY);
    res.gap = res.hpwlFinal - res.hpwlLower;
    res.overflowLower = overflow(occupancy(lower, lowerY));
    res.overflowFinal = overflow(occupancy(upper, upperY));
    if (fences != nullptr) {
        std::vector<double> flat;
        flat.reserve(2 * numMovable);
        for (std::size_t i = 0; i < numMovable; ++i) {
            flat.push_back(upper[i]);
            flat.push_back(upperY[i]);
        }
        res.fenceViolations = fences->countViolations(flat, regionOf);
    }

    for (std::size_t i = 0; i < numMovable; ++i) {
        outX[movVertex[i]] = upper[i];
        outY[movVertex[i]] = upperY[i];
    }

    writeBounds(bounds);
    writeFrame("FINAL_LAL", upper, upperY, "FINAL = last LAL (look-ahead legalized) placement",
               res.globalIters, std::max<std::size_t>(res.globalIters, 1));
    writeDensityMap("density_FINAL", res.globalIters, upper, upperY, "FINAL LAL");
    return res;
}

simplPlacer::simplPlacer(const Design &design) : pImpl(std::make_unique<Impl>(design)) {}
simplPlacer::~simplPlacer() = default;
simplPlacer::simplPlacer(simplPlacer &&) noexcept = default;
simplPlacer &simplPlacer::operator=(simplPlacer &&) noexcept = default;

SimplResult simplPlacer::place(const SimplOptions &options) {
    SimplResult result = pImpl->run(options);
    answer.xs = pImpl->outX;
    answer.ys = pImpl->outY;
    return result;
}

}  // namespace ktplace::simpl
