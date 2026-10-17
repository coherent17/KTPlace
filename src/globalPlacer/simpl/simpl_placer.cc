#include "globalPlacer/simpl/simpl_placer.h"

#include "globalPlacer/simpl/simpl_grid.h"
#include "globalPlacer/simpl/simpl_lal.h"
#include "globalPlacer/simpl/simpl_lss.h"
#include "globalPlacer/simpl/simpl_parallel.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace ktplace::simpl {

namespace {

std::string frameStep(std::size_t n) {
    std::string s = std::to_string(n);
    return std::string(s.size() < 4 ? 4 - s.size() : 0, '0') + s;
}

struct Positions {
    std::vector<double> x;
    std::vector<double> y;
};

enum class NetModel { Star, Bound2Bound };

struct InitialRounds {
    std::size_t accepted = 0;
    std::size_t framed = 0;
};

struct Convergence {
    double gapReference = 0.0;
    double bestUpper = std::numeric_limits<double>::max();
    std::size_t stalledFor = 0;

    bool update(std::size_t iteration, double gap, double upperHpwl, const SimplOptions &opts);
};

}  // namespace

class simplPlacer::Impl {
public:
    explicit Impl(Design &d) : design(d), graph(d.graph) {}

    SimplResult run(const SimplOptions &params);

private:
    Positions initialPlacement();
    void initialRounds(Positions &lower, NetModel model, InitialRounds &rounds);
    Positions globalPlacement(Positions &lower);
    void refine(Positions &lower, Positions &upper);
    void finish(const Positions &lower, const Positions &upper);

    void collect();
    void buildGrid(std::size_t perAxis);
    [[nodiscard]] std::size_t defaultBinsPerAxis() const;
    void seedUniform(std::vector<double> &x, std::vector<double> &y) const;

    [[nodiscard]] LinearSystems linearSystems() {
        return {.netlist = netlist, .sysX = sysX, .sysY = sysY, .floorX = floorX, .floorY = floorY};
    }
    void solve(std::vector<double> &x, std::vector<double> &y, const std::string &tag,
               std::size_t frameEvery);
    [[nodiscard]] double hpwl(const std::vector<double> &x, const std::vector<double> &y) const;

    [[nodiscard]] double centre(const std::vector<double> &pos, std::uint32_t i,
                                bool vertical) const;
    [[nodiscard]] std::vector<double> occupancy(const std::vector<double> &x,
                                                const std::vector<double> &y) const;
    [[nodiscard]] double overflow(const std::vector<double> &occ) const;
    void lookAheadLegalize(std::vector<double> &x, std::vector<double> &y) const;

    void enforceFences(std::vector<double> &x, std::vector<double> &y);

    void writeFrame(const std::string &series, const std::vector<double> &x,
                    const std::vector<double> &y, const std::string &note, std::size_t step,
                    std::size_t total);
    void writeBounds(std::span<const BoundsPoint> bounds) const;

    Design &design;
    const Graph &graph;
    const Fences *fences = nullptr;

    SimplOptions opts;
    SimplResult result;

    Netlist netlist;
    std::vector<double> width;
    std::vector<double> height;
    std::vector<double> area;
    std::vector<int> regionOf;
    std::vector<Box> obstacles;

    SparseSystem sysX;
    SparseSystem sysY;
    double floorX = 1.0;
    double floorY = 1.0;

    BinGrid grid;
    FreeSpace freeSpace;
    DieBox die{};
    double densityLimit = 1.0;
};

void simplPlacer::Impl::collect() {
    const std::size_t numCells = graph.getNumCells();
    netlist.varOfVertex.assign(numCells, noVar);
    netlist.fixedX.assign(numCells, 0.0);
    netlist.fixedY.assign(numCells, 0.0);
    for (std::size_t v = 0; v < numCells; ++v) {
        const Cell &cell = graph.getCell(v);
        netlist.fixedX[v] = cell.x;
        netlist.fixedY[v] = cell.y;
        if (cell.isFixed || cell.isTerminal) {
            if (cell.width > 0.0 && cell.height > 0.0) {
                obstacles.push_back({cell.x, cell.y, cell.x + cell.width, cell.y + cell.height});
            }
            ++result.numFixed;
            continue;
        }
        netlist.varOfVertex[v] = static_cast<std::uint32_t>(netlist.numMovable++);
        netlist.movVertex.push_back(static_cast<std::uint32_t>(v));
        width.push_back(cell.width);
        height.push_back(cell.height);
        area.push_back(cell.width * cell.height);
        regionOf.push_back(cell.regionId);
    }

    netlist.nets.resize(graph.getNumNets());
    for (std::size_t n = 0; n < netlist.nets.size(); ++n) {
        const std::vector<std::size_t> &pins = graph.getNetPins(n);
        std::vector<std::size_t> order(pins.begin(), pins.end());
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            return graph.getPin(a).cellId < graph.getPin(b).cellId;
        });
        NetPins &net = netlist.nets[n];
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

    netlist.pinStart.assign(netlist.numMovable + 1, 0);
    for (const NetPins &net : netlist.nets) {
        for (const std::uint32_t c : net.cells) {
            if (netlist.varOfVertex[c] != noVar) {
                ++netlist.pinStart[netlist.varOfVertex[c] + 1];
            }
        }
    }
    std::partial_sum(netlist.pinStart.begin(), netlist.pinStart.end(), netlist.pinStart.begin());
    netlist.pinsOfCell.resize(netlist.pinStart.back());
    std::vector<std::size_t> cursor(netlist.pinStart.begin(), netlist.pinStart.end() - 1);
    for (std::uint32_t n = 0; n < netlist.nets.size(); ++n) {
        for (std::uint32_t q = 0; q < netlist.nets[n].cells.size(); ++q) {
            const std::uint32_t v = netlist.varOfVertex[netlist.nets[n].cells[q]];
            if (v != noVar) {
                netlist.pinsOfCell[cursor[v]++] = {n, q};
            }
        }
    }

    result.numMovable = netlist.numMovable;
    result.nets = netlist.nets.size();
    if (netlist.numMovable > 0) {
        floorX = std::accumulate(width.begin(), width.end(), 0.0) /
                 static_cast<double>(netlist.numMovable);
        floorY = std::accumulate(height.begin(), height.end(), 0.0) /
                 static_cast<double>(netlist.numMovable);
    }
}

std::size_t simplPlacer::Impl::defaultBinsPerAxis() const {
    return opts.binsPerAxis > 0
               ? opts.binsPerAxis
               : static_cast<std::size_t>(std::clamp(
                     std::sqrt(static_cast<double>(std::max<std::size_t>(netlist.numMovable, 1)) /
                               51.0),
                     16.0, 256.0));
}

void simplPlacer::Impl::buildGrid(std::size_t perAxis) {
    die = design.die;
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

    std::vector<FreeSpace::Row> rows;
    for (const RowInfo &row : design.rows) {
        FreeSpace::Row r{row.coordinate, row.coordinate + row.height, {}};
        for (const Subrow &sub : row.subrows) {
            r.spans.emplace_back(sub.xlo, sub.xhi);
        }
        rows.push_back(std::move(r));
    }
    freeSpace.build(std::move(rows), obstacles);
}

void simplPlacer::Impl::seedUniform(std::vector<double> &x, std::vector<double> &y) const {
    std::mt19937_64 rng(opts.seed);
    const auto unit = [&rng] {
        return static_cast<double>(rng() >> 11) * 0x1.0p-53;
    };
    x.resize(netlist.numMovable);
    y.resize(netlist.numMovable);
    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        x[i] = die[0] + unit() * std::max(die[2] - die[0] - width[i], 0.0);
        y[i] = die[1] + unit() * std::max(die[3] - die[1] - height[i], 0.0);
    }
}


void simplPlacer::Impl::solve(std::vector<double> &x, std::vector<double> &y,
                              const std::string &tag, std::size_t frameEvery) {
    const auto observe = [&](std::size_t iterate, const Residuals &residuals, bool last) {
        if (frameEvery > 0 && (iterate % frameEvery == 0 || last)) {
            writeFrame("cg_" + tag, x, y,
                       std::format("CG {} iterate {}, residual x {:.3e} / y {:.3e}", tag, iterate,
                                   residuals.x, residuals.y),
                       iterate, opts.cgMaxIter);
        }
    };
    const Residuals residuals =
        solveBothAxes(sysX, x, sysY, y, opts.cgTol, opts.cgMaxIter, observe);
    ktlog.trace("  cg {}: residual x {:.3e} y {:.3e}", tag, residuals.x, residuals.y);
}

double simplPlacer::Impl::hpwl(const std::vector<double> &x, const std::vector<double> &y) const {
    std::vector<double> px = netlist.fixedX;
    std::vector<double> py = netlist.fixedY;
    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        px[netlist.movVertex[i]] = x[i];
        py[netlist.movVertex[i]] = y[i];
    }
    return deterministicSum(netlist.nets.size(), [&](std::size_t n) {
        const NetPins &net = netlist.nets[n];
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
    for (std::uint32_t i = 0; i < netlist.numMovable; ++i) {
        occ[grid.binOf(centre(x, i, true), centre(y, i, false))] += area[i];
    }
    return occ;
}

double simplPlacer::Impl::overflow(const std::vector<double> &occ) const {
    double excess = 0.0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        excess += std::max(occ[k] - densityLimit * grid.avail[k], 0.0);
    }
    return grid.totalAvail > 0.0 ? excess / grid.totalAvail : 0.0;
}

void simplPlacer::Impl::lookAheadLegalize(std::vector<double> &x, std::vector<double> &y) const {
    const LookAheadLegalizer legalizer{.grid = grid,
                                       .freeSpace = freeSpace,
                                       .obstacles = obstacles,
                                       .width = width,
                                       .height = height,
                                       .area = area,
                                       .opts = opts,
                                       .densityLimit = densityLimit,
                                       .die = die};
    legalizer.legalize(x, y);
}

void simplPlacer::Impl::enforceFences(std::vector<double> &x, std::vector<double> &y) {
    if (fences == nullptr || fences->numRegions() == 0) {
        return;
    }
    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        if (regionOf[i] != Fences::noRegion) {
            result.fenceClamps += fences->clampToRegion(regionOf[i], x[i], y[i]) ? 1 : 0;
        } else {
            result.fencePushes +=
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
    std::vector<float> fx(netlist.fixedX.begin(), netlist.fixedX.end());
    std::vector<float> fy(netlist.fixedY.begin(), netlist.fixedY.end());
    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        fx[netlist.movVertex[i]] = static_cast<float>(x[i]);
        fy[netlist.movVertex[i]] = static_cast<float>(y[i]);
    }
    FrameInfo info;
    info.note = note;
    info.series = series;
    info.step = step;
    info.numSteps = total;
    info.hpwl = hpwl(x, y);
    info.hpwlInitial = result.hpwlSeed;
    info.overflow = overflow(occupancy(x, y));
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

void simplPlacer::Impl::writeBounds(std::span<const BoundsPoint> bounds) const {
    if (design.onBounds) {
        design.onBounds(bounds);
    }
}

SimplResult simplPlacer::Impl::run(const SimplOptions &params) {
    opts = params;
    densityLimit = std::clamp(opts.densityLimit, 0.05, 1.0);
    fences = params.fences ? &design.fences : nullptr;
    result.fenceRegions = fences != nullptr ? fences->numRegions() : 0;

    {
        ScopedTimer setup("simpl-setup");
        collect();
        buildGrid(defaultBinsPerAxis());
        result.binsX = grid.nx;
        result.binsY = grid.ny;
    }

    if (netlist.numMovable == 0) {
        return result;
    }

    Positions lower = initialPlacement();
    Positions upper = globalPlacement(lower);
    finish(lower, upper);
    return result;
}

Positions simplPlacer::Impl::initialPlacement() {
    Positions lower;
    seedUniform(lower.x, lower.y);
    result.hpwlSeed = hpwl(lower.x, lower.y);
    ktlog.echo("grid {}x{}, available area {:.4g}, density limit g = {:.2f}, seed {} (HPWL {:.6e})",
               grid.nx, grid.ny, grid.totalAvail, densityLimit, opts.seed, result.hpwlSeed);
    writeFrame("LSS_init_seed", lower.x, lower.y, "initial placement: uniform seed", 0,
               opts.initMaxIters);

    result.hpwlInit = result.hpwlSeed;
    InitialRounds rounds;
    if (opts.initialNetModel != "b2b") {
        initialRounds(lower, NetModel::Star, rounds);
    }
    if (opts.initialNetModel != "star") {
        initialRounds(lower, NetModel::Bound2Bound, rounds);
    }
    if (rounds.accepted > 0 && rounds.framed != rounds.accepted) {
        writeFrame("LSS_init", lower.x, lower.y,
                   std::format("initial placement round {}", rounds.accepted), rounds.accepted,
                   opts.initMaxIters);
    }
    return lower;
}

void simplPlacer::Impl::initialRounds(Positions &lower, NetModel model, InitialRounds &rounds) {
    const bool star = model == NetModel::Star;
    const std::vector<double> noAnchors;
    Positions extended;
    if (star) {
        ScopedTimer timer("simpl-init");
        const std::size_t size = linearSystems().buildStar();
        extended = lower;
        extended.x.resize(size, 0.5 * (die[0] + die[2]));
        extended.y.resize(size, 0.5 * (die[1] + die[3]));
    }
    for (std::size_t round = 0; round < opts.initMaxIters; ++round) {
        ScopedTimer timer("simpl-init");
        Positions next = star ? extended : lower;
        if (!star) {
            linearSystems().buildB2B(next.x, next.y, 0.0, noAnchors, noAnchors);
        }
        solve(next.x, next.y, "init" + frameStep(result.initIters),
              result.initIters == 0 ? opts.initCgEvery : 0);
        if (star) {
            extended = next;
            next.x.resize(netlist.numMovable);
            next.y.resize(netlist.numMovable);
        }
        const double roundHpwl = hpwl(next.x, next.y);
        ktlog.trace("init iter {:2d} ({}): hpwl {:.6e}", result.initIters, star ? "star" : "b2b",
                    roundHpwl);
        ++result.initIters;
        const bool first = star && round == 0;
        if (!first && !(roundHpwl < result.hpwlInit)) {
            break;
        }
        lower = std::move(next);
        result.hpwlInit = roundHpwl;
        rounds.accepted = result.initIters;
        if ((rounds.accepted & (rounds.accepted - 1)) == 0) {
            rounds.framed = rounds.accepted;
            writeFrame("LSS_init", lower.x, lower.y,
                       std::format("initial placement round {}", rounds.accepted), rounds.accepted,
                       opts.initMaxIters);
        }
    }
}

bool Convergence::update(std::size_t iteration, double gap, double upperHpwl,
                         const SimplOptions &opts) {
    if (iteration == opts.gapReferenceIter) {
        gapReference = gap;
    }
    stalledFor = upperHpwl < bestUpper ? 0 : stalledFor + 1;
    bestUpper = std::min(bestUpper, upperHpwl);
    const bool upperStalled = stalledFor >= opts.stallIters;
    return iteration > opts.gapReferenceIter && gapReference > 0.0 &&
           (gap < opts.gapTightFrac * gapReference ||
            (gap <= opts.gapRelaxedFrac * gapReference && upperStalled));
}

Positions simplPlacer::Impl::globalPlacement(Positions &lower) {
    Positions upper;
    std::vector<BoundsPoint> bounds;
    Convergence convergence;
    for (std::size_t it = 0; it < opts.maxIters; ++it) {
        result.globalIters = it + 1;
        {
            ScopedTimer timer("simpl-legalize");
            upper = lower;
            lookAheadLegalize(upper.x, upper.y);
            enforceFences(upper.x, upper.y);
            result.spreadSeconds += timer.elapsedSeconds();
        }

        const double upperHpwl = hpwl(upper.x, upper.y);
        const double lowerHpwl = hpwl(lower.x, lower.y);
        const double gap = upperHpwl - lowerHpwl;
        bounds.push_back({upperHpwl, lowerHpwl});
        ktlog.trace("iter {:3d}: lower {:.6e} upper {:.6e} gap {:.4e} overflow {:.4f} -> {:.4f}",
                    it, lowerHpwl, upperHpwl, gap, overflow(occupancy(lower.x, lower.y)),
                    overflow(occupancy(upper.x, upper.y)));
        if (opts.traceEvery > 0 && it % opts.traceEvery == 0) {
            writeFrame("LSS", lower.x, lower.y,
                       std::format("LSS (linear system solve) - iteration {}", it), it,
                       opts.maxIters);
            writeFrame("LAL", upper.x, upper.y,
                       std::format("LAL (look-ahead legalized) - iteration {}", it), it,
                       opts.maxIters);
        }

        if (convergence.update(it + 1, gap, upperHpwl, opts)) {
            ktlog.trace("converged at iteration {}: gap {:.4e}, reference {:.4e}", it, gap,
                        convergence.gapReference);
            break;
        }

        const double alpha = opts.alphaBase * (1.0 + static_cast<double>(it));
        {
            ScopedTimer timer("simpl-build");
            linearSystems().buildB2B(lower.x, lower.y, alpha, upper.x, upper.y);
            result.buildSeconds += timer.elapsedSeconds();
        }
        {
            ScopedTimer timer("simpl-solve");
            solve(lower.x, lower.y, "g" + frameStep(it), opts.cgEvery);
            result.solveSeconds += timer.elapsedSeconds();
        }
    }

    if (!upper.x.empty() && opts.refineIters > 0) {
        refine(lower, upper);
    }
    if (upper.x.empty()) {
        upper = lower;
        enforceFences(upper.x, upper.y);
    }
    writeBounds(bounds);
    return upper;
}

void simplPlacer::Impl::refine(Positions &lower, Positions &upper) {
    const std::size_t done = result.globalIters;
    buildGrid(grid.nx * opts.refineGridFactor);
    for (std::size_t r = 0; r < opts.refineIters; ++r) {
        const double alpha = opts.alphaBase * (1.0 + static_cast<double>(done + r));
        linearSystems().buildB2B(lower.x, lower.y, alpha, upper.x, upper.y);
        solve(lower.x, lower.y, "refine" + frameStep(r), opts.cgEvery);
        upper = lower;
        lookAheadLegalize(upper.x, upper.y);
        enforceFences(upper.x, upper.y);
        ktlog.trace("refine {:2d} on {}x{} bins: lower {:.6e} upper {:.6e} overflow {:.4f}", r,
                    grid.nx, grid.ny, hpwl(lower.x, lower.y), hpwl(upper.x, upper.y),
                    overflow(occupancy(upper.x, upper.y)));
    }
}

void simplPlacer::Impl::finish(const Positions &lower, const Positions &upper) {
    result.hpwlFinal = hpwl(upper.x, upper.y);
    result.hpwlLower = hpwl(lower.x, lower.y);
    result.gap = result.hpwlFinal - result.hpwlLower;
    result.overflowLower = overflow(occupancy(lower.x, lower.y));
    result.overflowFinal = overflow(occupancy(upper.x, upper.y));
    if (fences != nullptr) {
        std::vector<double> flat;
        flat.reserve(2 * netlist.numMovable);
        for (std::size_t i = 0; i < netlist.numMovable; ++i) {
            flat.push_back(upper.x[i]);
            flat.push_back(upper.y[i]);
        }
        result.fenceViolations = fences->countViolations(flat, regionOf);
    }

    for (std::size_t i = 0; i < netlist.numMovable; ++i) {
        design.graph.setPosition(netlist.movVertex[i], upper.x[i], upper.y[i]);
    }

    writeFrame("FINAL_LAL", upper.x, upper.y, "FINAL = last LAL (look-ahead legalized) placement",
               result.globalIters, std::max<std::size_t>(result.globalIters, 1));
}

simplPlacer::simplPlacer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}
simplPlacer::~simplPlacer() = default;
simplPlacer::simplPlacer(simplPlacer &&) noexcept = default;
simplPlacer &simplPlacer::operator=(simplPlacer &&) noexcept = default;

SimplResult simplPlacer::place(const SimplOptions &options) {
    return pImpl->run(options);
}

}  // namespace ktplace::simpl
