/**
 * @file kt_quadPlacer.cc
 * @brief TBB-parallel quadratic placement implementation
 *
 * Net model: clique for nets with <= kStarThreshold distinct cells,
 * star nodes otherwise (avoids O(k^2) blow-up on large nets).
 *
 * Linear algebra:
 *   - Sparse matrix kept in CSR form (full symmetric, off-diagonal terms)
 *   - Diagonal stored as a dense array
 *   - Solve with Jacobi-preconditioned Conjugate Gradient
 *
 * All assembly and solver kernels use oneTBB parallel_for / parallel_reduce.
 */

#include "placer/kt_quadPlacer.h"
#include "datamodel/kt_graph.h"
#include "constraint/kt_constraintMgr.h"
#include "util/kt_log.h"
#include "visualization/kt_plotter.h"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <oneapi/tbb/partitioner.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ktplace {

namespace {

constexpr std::size_t kStarThreshold = 32;
constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();
constexpr double kReg = 1e-6;  // diagonal regularization (handles disconnected components)

// Inner (per outer iteration) CG budget and relative tolerance.
constexpr int kInnerMaxIter = 40;
constexpr double kInnerTol = 0.05;
// Per-iteration cap on the projection displacement, in density-bin widths.
// Capping keeps the wirelength solve able to keep net clusters intact while
// the outward walk still drains over-full regions over many iterations.
constexpr double kMaxStepBins = 1.0;
// Trust region on the refinement solve, in bin widths: the most a single cell
// may move per outer iteration.
constexpr double kMaxMoveBins = 8.0;
// Weight of the bin-density gradient in the solve's right-hand side. The solve
// otherwise only knows wirelength, so a crowded bin exerts no force at all and
// the placement has to be dragged apart after the fact by the capped direct
// spread, which is too weak to keep up.
//
// The weight is ramped geometrically across the refinement phase rather than
// held fixed. Early on, density is far from uniform and a strong force only
// fights the wirelength term and lengthens nets; late on, the weight is large
// so the solve drives the last of the overdensity out and the run ends with the
// grid barely overdense. A constant weight has to compromise between those two
// and ends up too weak to converge (or too strong to route at all).
constexpr double kDensityGradStart = 4.0;
constexpr double kDensityGradEnd = 600.0;
// Pull strength of the projection targets inside the coupled RHS.  Deliberately
// weak: it only shifts the WL equilibrium a little toward the drain target, so
// the wirelength solve keeps the clusters coherent while the (read-only) bias
// lets the capped nudge accumulate into a steady outward drift.
constexpr double kProjMu = 2.0;

// Outer iterations reserved for the pure-projection pre-spread phase (see
// PLACER loop).  Covers the collapsed die-center seed before refining.
// Fraction of the outer-iteration budget spent spreading before the
// wirelength-refinement phase begins. Expressed as a fraction of the budget
// rather than a fixed count, so the refinement window is never empty whatever
// budget the caller asks for.
constexpr double kSpreadPhaseFraction = 0.5;
// Minimum outer iterations before early termination on density overflow.
constexpr std::size_t kOuterWarmup = 16;

// CSR sparse matrix (off-diagonal entries only; diagonal kept separately).
struct CsrMatrix {
    std::size_t n = 0;
    std::vector<std::size_t> rowPtr;
    std::vector<std::size_t> col;
    std::vector<double> val;
    std::vector<double> diag;
};

// Collect, sort and de-duplicate the distinct cell vertex IDs of a net.
inline std::vector<std::size_t> netCellIds(const Graph &g, const Vertex &net) {
    std::vector<std::size_t> ids;
    ids.reserve(net.inEdges.size());
    for (std::size_t eid : net.inEdges) {
        ids.push_back(g.getEdge(eid).source);
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// Deterministic chunked reduction. TBB's own parallel_reduce combines partials
// in an internal tree whose shape depends on runtime scheduling, so the same
// binary produced different placements on different runs (measured drift of
// ~0.03 in density overflow). Here the index space is split into a fixed number
// of contiguous ranges, each processed by exactly one thread, and the partials
// are combined in chunk order -- bitwise deterministic for a fixed machine.
struct DeterministicChunks {
    const std::size_t n;
    const std::size_t nChunks;
    const std::size_t grain;
    explicit DeterministicChunks(std::size_t total)
        : n(total),
          nChunks(std::max<std::size_t>(1, tbb::this_task_arena::max_concurrency())),
          grain((n + nChunks - 1) / nChunks) {}
    std::size_t begin(std::size_t c) const { return c * grain; }
    std::size_t end(std::size_t c) const { return std::min((c + 1) * grain, n); }
};

template <typename Body>
double deterministicReduce(std::size_t n, Body &&body) {
    const DeterministicChunks ck(n);
    std::vector<double> partial(ck.nChunks, 0.0);
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, ck.nChunks, ck.nChunks),
        [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t c = r.begin(); c != r.end(); ++c) {
                double acc = 0.0;
                body(ck.begin(c), ck.end(c), acc);
                partial[c] = acc;
            }
        },
        tbb::simple_partitioner{});
    double total = 0.0;
    for (std::size_t c = 0; c < ck.nChunks; ++c) {
        total += partial[c];
    }
    return total;
}

// NTUplace bell-shaped density kernel. Piecewise quadratic with support
// [-1, 1] bin widths and a continuous first derivative at the 0.5 join. Sampled
// at the bin centers it is a partition of unity, so splatting a cell over its
// 3x3 neighborhood conserves the cell's area exactly while keeping the density
// field C1 (the per-bin density changes continuously as a cell moves -- the
// bilinear kernel was only C0, so its gradient jumped at every bin boundary).
inline double densityBell(double u) {
    u = std::abs(u);
    if (u <= 0.5) return 1.0 - 2.0 * u * u;
    if (u <= 1.0) return 2.0 * (1.0 - u) * (1.0 - u);
    return 0.0;
}

// Sparse matvec: out = diag o x + A_off * x
void matvec(const CsrMatrix &m, const std::vector<double> &x, std::vector<double> &out) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, m.n),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              double acc = 0.0;
                              const std::size_t begin = m.rowPtr[i];
                              const std::size_t end = m.rowPtr[i + 1];
                              for (std::size_t k = begin; k < end; ++k) {
                                  acc += m.val[k] * x[m.col[k]];
                              }
                              out[i] = m.diag[i] * x[i] + acc;
                          }
                      });
}

double dot(const std::vector<double> &a, const std::vector<double> &b) {
    return deterministicReduce(a.size(), [&](std::size_t b0, std::size_t e0, double &acc) {
        for (std::size_t i = b0; i < e0; ++i) {
            acc += a[i] * b[i];
        }
    });
}

double norm2(const std::vector<double> &v) {
    return std::sqrt(dot(v, v));
}

// Jacobi-preconditioned CG. x is the warm-start guess, updated in place.
// If onIter is set, it is invoked after every completed iteration with the
// 1-based iteration number, the current solution, its relative residual, and
// isFinal=true for the last (converged or exhausted-budget) state.
using IterCb =
    std::function<void(std::size_t it, const std::vector<double> &x, double resid, bool isFinal)>;
void conjugateGradient(const CsrMatrix &m, const std::vector<double> &b, std::vector<double> &x,
                       int maxIter, double tol, std::size_t &itOut, double &resOut,
                       const IterCb &onIter = {}) {
    const std::size_t n = x.size();
    if (n == 0) {
        itOut = 0;
        resOut = 0.0;
        return;
    }

    // Preconditioner: M^{-1} = 1/diag (regularized)
    std::vector<double> invDiag(n);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                      [&](const tbb::blocked_range<std::size_t> &br) {
                          for (std::size_t i = br.begin(); i != br.end(); ++i) {
                              invDiag[i] = 1.0 / std::max(m.diag[i], kReg);
                          }
                      });

    std::vector<double> r(n), z(n), p(n), q(n);
    const double bNorm = norm2(b);
    if (bNorm == 0.0) {
        std::fill(x.begin(), x.end(), 0.0);
        itOut = 0;
        resOut = 0.0;
        return;
    }

    matvec(m, x, r);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                      [&](const tbb::blocked_range<std::size_t> &r2) {
                          for (std::size_t i = r2.begin(); i != r2.end(); ++i) {
                              r[i] = b[i] - r[i];
                          }
                      });
    double rho = 0.0;
    {
        std::vector<double> tmp(n);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                          [&](const tbb::blocked_range<std::size_t> &r3) {
                              for (std::size_t i = r3.begin(); i != r3.end(); ++i) {
                                  tmp[i] = invDiag[i] * r[i];
                              }
                          });
        rho = dot(r, tmp);
        z = std::move(tmp);
        p = z;
    }

    std::size_t it = 0;
    double resn = std::numeric_limits<double>::max();
    for (; it < static_cast<std::size_t>(maxIter); ++it) {
        matvec(m, p, q);
        const double pAq = dot(p, q);
        const double alpha = rho / std::max(pAq, std::numeric_limits<double>::min());

        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                          [&](const tbb::blocked_range<std::size_t> &r4) {
                              for (std::size_t i = r4.begin(); i != r4.end(); ++i) {
                                  x[i] += alpha * p[i];
                                  r[i] -= alpha * q[i];
                              }
                          });

        resn = norm2(r) / bNorm;
        if (onIter) {
            onIter(it + 1, x, resn, false);
        }
        if (resn <= tol) {
            break;
        }

        // z = M^{-1} r
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                          [&](const tbb::blocked_range<std::size_t> &r5) {
                              for (std::size_t i = r5.begin(); i != r5.end(); ++i) {
                                  z[i] = invDiag[i] * r[i];
                              }
                          });
        const double rhoNew = dot(r, z);
        const double beta = rhoNew / std::max(rho, std::numeric_limits<double>::min());
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                          [&](const tbb::blocked_range<std::size_t> &r6) {
                              for (std::size_t i = r6.begin(); i != r6.end(); ++i) {
                                  p[i] = z[i] + beta * p[i];
                              }
                          });
        rho = rhoNew;
    }

    itOut = it + 1;
    resOut = resn;
    if (onIter) {
        onIter(itOut, x, resn, true);
    }
}

// Half-perimeter wirelength over the netlist given per-cell coordinate arrays.
double hpwl(const Graph &g, const std::vector<double> &x, const std::vector<double> &y) {
    const std::size_t nv = g.getNumVertices();
    return deterministicReduce(nv, [&](std::size_t b0, std::size_t e0, double &acc) {
        for (std::size_t v = b0; v < e0; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Net || vert.inEdges.empty()) {
                continue;
            }
            double minX = std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double minY = std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            for (std::size_t eid : vert.inEdges) {
                const std::size_t cid = g.getEdge(eid).source;
                minX = std::min(minX, x[cid]);
                maxX = std::max(maxX, x[cid]);
                minY = std::min(minY, y[cid]);
                maxY = std::max(maxY, y[cid]);
            }
            const double span = (maxX - minX) + (maxY - minY);
            acc += (span >= 0.0 ? span : 0.0) * vert.weight;
        }
    });
}

// hpwl() over float per-vertex coordinate arrays (used by the frame plotter).
double hpwlF(const Graph &g, const std::vector<float> &x, const std::vector<float> &y) {
    const std::size_t nv = g.getNumVertices();
    return deterministicReduce(nv, [&](std::size_t b0, std::size_t e0, double &acc) {
        for (std::size_t v = b0; v < e0; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Net || vert.inEdges.empty()) {
                continue;
            }
            double minX = std::numeric_limits<double>::max();
            double maxX = -std::numeric_limits<double>::max();
            double minY = std::numeric_limits<double>::max();
            double maxY = -std::numeric_limits<double>::max();
            for (std::size_t eid : vert.inEdges) {
                const std::size_t cid = g.getEdge(eid).source;
                minX = std::min(minX, static_cast<double>(x[cid]));
                maxX = std::max(maxX, static_cast<double>(x[cid]));
                minY = std::min(minY, static_cast<double>(y[cid]));
                maxY = std::max(maxY, static_cast<double>(y[cid]));
            }
            const double span = (maxX - minX) + (maxY - minY);
            acc += (span >= 0.0 ? span : 0.0) * vert.weight;
        }
    });
}

// ---------------------------------------------------------------------------
// Density grid used for spreading and for the overflow metric: a uniform bin
// grid over the die. Cells are spread with a SimPL-style projection: each
// movable cell is assigned an area-balanced target bin (sorted scanne /area
// fill), then pulled toward it; over-packed regions drain into empty ones.
// ---------------------------------------------------------------------------
/// Pool id for the part of the die that is not fenced off.
constexpr int kFreePool = -1;

/// Pool id for a bin that straddles a fence edge: usable by neither the fenced
/// cells nor the unconstrained ones.
constexpr int kBlockedPool = -2;

struct DensityGrid {
    int nbx = 8;
    int nby = 8;
    double x0 = 0.0;
    double y0 = 0.0;
    double dx = 1.0;
    double dy = 1.0;
    std::vector<double> occ;  // accumulated cell area per bin

    // Usable area per bin: the bin area minus whatever is already taken by
    // fixed macros, terminals and fence interiors. Without this the equi-area
    // map happily aims movable cells at blocked area, and since the wirelength
    // solve pushes them back out they collect at the first free spot -- which
    // is how a whole population ends up piled in one corner on top of a macro.
    std::vector<double> cap;

    // Downhill slope of the per-bin utilization, refreshed with the occupancy.
    // This is the density force the wirelength solve needs: without it the only
    // thing in the system that knows about density is the projection spring, and
    // a cell sitting in a crowded bin feels nothing, so the whole placement
    // drifts together instead of draining. Stored as a per-bin difference of
    // utilization (dimensionless) so the force does not depend on bin size.
    std::vector<double> gradX;
    std::vector<double> gradY;

    // Region-constraint support: the placement region of each movable cell
    // (kNoRegion when unconstrained), plus bin -> pool id and the bins of each
    // pool in raster order. All empty when unconstrained.
    std::vector<int> regionOfMov;
    std::vector<int> binPool;
    std::vector<std::vector<std::uint32_t>> poolBins;

    std::size_t idx(int ix, int iy) const {
        return static_cast<std::size_t>(iy) * static_cast<std::size_t>(nbx) +
               static_cast<std::size_t>(ix);
    }
};

// Uniform bin grid covering the die bounding box.
//
// The resolution is derived from the design instead of being a fixed 64 across:
// what matters is how many cells land in one bin. A bin is sized so it averages
// kTargetCellsPerBin cells, which keeps the density estimate meaningful while
// making the bin as fine as the cell count allows. Pinning 64 bins across made
// the effective resolution vary about 30x across this benchmark suite -- a
// 4 um bin on mgc_fft_1 but a 32 um bin on mgc_superblue11_a, where a single
// bin swallowed ~150 cells on a side and the per-bin density gradient was
// effectively constant over 1000 um, too coarse to push anything apart.
//
// The floor keeps tiny designs from degenerating and the ceiling bounds the
// per-iteration O(bins) work (refresh, gradient, prefix sum).
DensityGrid makeDensityGrid(const BBox &die, std::size_t numCells) {
    const double w = std::max(die[2] - die[0], 1.0);
    const double h = std::max(die[3] - die[1], 1.0);
    constexpr double kTargetCellsPerBin = 32.0;
    constexpr int kMinBinsPerAxis = 32;
    constexpr int kMaxBinsPerAxis = 512;

    // Bin edge that averages kTargetCellsPerBin cells over the whole die.
    const double binArea = (w * h) * kTargetCellsPerBin / static_cast<double>(std::max<std::size_t>(numCells, 1));
    const double edge = std::sqrt(std::max(binArea, 1.0));
    const auto axis = [&](double extent) {
        const int n = static_cast<int>(std::lround(extent / edge));
        return std::clamp(n, kMinBinsPerAxis, kMaxBinsPerAxis);
    };
    const int nbx = axis(w);
    const int nby = axis(h);
    DensityGrid g;
    g.nbx = nbx;
    g.nby = nby;
    g.x0 = die[0];
    g.y0 = die[1];
    g.dx = w / static_cast<double>(nbx);
    g.dy = h / static_cast<double>(nby);
    g.occ.assign(static_cast<std::size_t>(nbx) * nby, 0.0);
    g.cap.assign(g.occ.size(), g.dx * g.dy);
    g.binPool.assign(g.occ.size(), kFreePool);
    g.poolBins.clear();
    g.regionOfMov.clear();
    return g;
}

// Partition the bin grid into one pool per placement region plus a pool for
// the free area, so the equi-area projection can lay each group out inside its
// own fence.  Done once per run: the grid never moves.
void buildRegionPools(DensityGrid &g, const constraintMgr &constraints) {
    const std::size_t regions = constraints.numRegions();
    g.binPool.assign(g.occ.size(), kFreePool);
    g.poolBins.assign(regions + 1, {});
    for (std::size_t iy = 0; iy < static_cast<std::size_t>(g.nby); ++iy) {
        for (std::size_t ix = 0; ix < static_cast<std::size_t>(g.nbx); ++ix) {
            const std::size_t k = g.idx(static_cast<int>(ix), static_cast<int>(iy));
            const double cx = g.x0 + (static_cast<double>(ix) + 0.5) * g.dx;
            const double cy = g.y0 + (static_cast<double>(iy) + 0.5) * g.dy;
            int pool = kFreePool;
            bool blocked = false;
            for (std::size_t r = 0; r < regions; ++r) {
                if (constraints.contains(static_cast<int>(r), cx, cy)) {
                    pool = static_cast<int>(r);
                    break;
                }
            }
            if (pool == kFreePool) {
                // A fence is reserved for its own cells, so a bin that only
                // *partly* overlaps a fence must not be offered to unconstrained
                // cells: the equi-area walk would target them straight onto the
                // fence. Such bins are left out of the free pool entirely.
                const double bx0 = g.x0 + static_cast<double>(ix) * g.dx;
                const double by0 = g.y0 + static_cast<double>(iy) * g.dy;
                for (std::size_t r = 0; r < regions; ++r) {
                    for (const Rect &rc : constraints.region(static_cast<int>(r))->rects) {
                        if (bx0 <= rc.hi.x && bx0 + g.dx >= rc.lo.x && by0 <= rc.hi.y &&
                            by0 + g.dy >= rc.lo.y) {
                            g.binPool[k] = kBlockedPool;
                            blocked = true;
                            break;
                        }
                    }
                    if (blocked) {
                        break;
                    }
                }
            }
            if (blocked) {
                continue;  // bin belongs to no pool
            }
            g.binPool[k] = pool;
            // kFreePool is -1; the free pool is stored last.
            const std::size_t poolIndex =
                (pool == kFreePool) ? regions : static_cast<std::size_t>(pool);
            g.poolBins[poolIndex].push_back(static_cast<std::uint32_t>(k));
        }
    }
}

// Subtract the area that movable cells can never use: fixed macros, terminals
// and placement blockages, plus the part of a bin that a fence reserves.
void applyBlockage(DensityGrid &g, const Graph &graph, const constraintMgr *constraints) {
    for (std::size_t v = 0; v < graph.getNumVertices(); ++v) {
        const Vertex &vert = graph.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        if (!vert.isFixed && !vert.isTerminal) {
            continue;  // movable area is exactly what we are placing
        }
        const double w = std::max(vert.width, 0.0);
        const double h = std::max(vert.height, 0.0);
        if (!(w > 0.0) || !(h > 0.0)) {
            continue;
        }
        // Clip to the grid, then bill the overlap to the covered bins.
        const double x0 = std::max(vert.x, g.x0);
        const double y0 = std::max(vert.y, g.y0);
        const double x1 = std::min(vert.x + w, g.x0 + g.dx * g.nbx);
        const double y1 = std::min(vert.y + h, g.y0 + g.dy * g.nby);
        if (!(x1 > x0) || !(y1 > y0)) {
            continue;
        }
        const int ix0 = std::clamp(static_cast<int>((x0 - g.x0) / g.dx), 0, g.nbx - 1);
        const int iy0 = std::clamp(static_cast<int>((y0 - g.y0) / g.dy), 0, g.nby - 1);
        const int ix1 = std::clamp(static_cast<int>((x1 - g.x0) / g.dx), 0, g.nbx - 1);
        const int iy1 = std::clamp(static_cast<int>((y1 - g.y0) / g.dy), 0, g.nby - 1);
        for (int iy = iy0; iy <= iy1; ++iy) {
            const double by0 = g.y0 + static_cast<double>(iy) * g.dy;
            const double by1 = by0 + g.dy;
            const double oy = std::min(by1, y1) - std::max(by0, y0);
            if (!(oy > 0.0)) {
                continue;
            }
            for (int ix = ix0; ix <= ix1; ++ix) {
                const double bx0 = g.x0 + static_cast<double>(ix) * g.dx;
                const double bx1 = bx0 + g.dx;
                const double ox = std::min(bx1, x1) - std::max(bx0, x0);
                if (!(ox > 0.0)) {
                    continue;
                }
                g.cap[g.idx(ix, iy)] = std::max(g.cap[g.idx(ix, iy)] - ox * oy, 0.0);
            }
        }
    }

    if (constraints == nullptr) {
        return;
    }
    // A fence reserves its area from unconstrained cells, so only the remainder
    // of a straddling bin is up for grabs.
    for (std::size_t r = 0; r < constraints->numRegions(); ++r) {
        for (const Rect &rc : constraints->region(static_cast<int>(r))->rects) {
            const double rx0 = std::max(rc.lo.x, g.x0);
            const double ry0 = std::max(rc.lo.y, g.y0);
            const double rx1 = std::min(rc.hi.x, g.x0 + g.dx * g.nbx);
            const double ry1 = std::min(rc.hi.y, g.y0 + g.dy * g.nby);
            if (!(rx1 > rx0) || !(ry1 > ry0)) {
                continue;
            }
            const int ix0 = std::clamp(static_cast<int>((rx0 - g.x0) / g.dx), 0, g.nbx - 1);
            const int iy0 = std::clamp(static_cast<int>((ry0 - g.y0) / g.dy), 0, g.nby - 1);
            const int ix1 = std::clamp(static_cast<int>((rx1 - g.x0) / g.dx), 0, g.nbx - 1);
            const int iy1 = std::clamp(static_cast<int>((ry1 - g.y0) / g.dy), 0, g.nby - 1);
            for (int iy = iy0; iy <= iy1; ++iy) {
                const double oy = std::min(g.y0 + (iy + 1) * g.dy, ry1) -
                                  std::max(g.y0 + iy * g.dy, ry0);
                if (!(oy > 0.0)) {
                    continue;
                }
                for (int ix = ix0; ix <= ix1; ++ix) {
                    const double ox = std::min(g.x0 + (ix + 1) * g.dx, rx1) -
                                      std::max(g.x0 + ix * g.dx, rx0);
                    if (!(ox > 0.0)) {
                        continue;
                    }
                    g.cap[g.idx(ix, iy)] = std::max(g.cap[g.idx(ix, iy)] - ox * oy, 0.0);
                }
            }
        }
    }
}

// Downhill utilization slope per bin: (u[k+1] - u[k-1]) / 2 along each axis,
// one-sided at the border. u is occupancy over usable capacity, so a bin that is
// completely blocked by a macro reads as 0 and contributes no force.
void refreshDensityGradient(DensityGrid &g) {
    const std::size_t n = g.occ.size();
    g.gradX.assign(n, 0.0);
    g.gradY.assign(n, 0.0);
    for (std::size_t iy = 0; iy < static_cast<std::size_t>(g.nby); ++iy) {
        for (std::size_t ix = 0; ix < static_cast<std::size_t>(g.nbx); ++ix) {
            const std::size_t k = g.idx(static_cast<int>(ix), static_cast<int>(iy));
            const double u = g.cap[k] > 0.0 ? g.occ[k] / g.cap[k] : 0.0;
            const std::size_t kxm =
                (ix > 0) ? g.idx(static_cast<int>(ix - 1), static_cast<int>(iy)) : k;
            const std::size_t kxp =
                (ix + 1 < g.nbx) ? g.idx(static_cast<int>(ix + 1), static_cast<int>(iy)) : k;
            const std::size_t kym =
                (iy > 0) ? g.idx(static_cast<int>(ix), static_cast<int>(iy - 1)) : k;
            const std::size_t kyp =
                (iy + 1 < g.nby) ? g.idx(static_cast<int>(ix), static_cast<int>(iy + 1)) : k;
            const double uxm = g.cap[kxm] > 0.0 ? g.occ[kxm] / g.cap[kxm] : 0.0;
            const double uxp = g.cap[kxp] > 0.0 ? g.occ[kxp] / g.cap[kxp] : 0.0;
            const double uym = g.cap[kym] > 0.0 ? g.occ[kym] / g.cap[kym] : 0.0;
            const double uyp = g.cap[kyp] > 0.0 ? g.occ[kyp] / g.cap[kyp] : 0.0;
            g.gradX[k] = 0.5 * (uxp - uxm);
            g.gradY[k] = 0.5 * (uyp - uym);
            (void)u;
        }
    }
}

// Accumulate movable cell area into a C1-smooth density field with the NTUplace
// bell-shaped kernel. Each cell splats across the up-to-3x3 bin neighborhood of
// its center with separable quadratic weights; x/y hold the full solve vector,
// only the first nMov entries participate. The accumulation is chunked by cell
// index into partial bins combined in chunk order, so the field is bitwise
// reproducible (and the area conservation is exact, since the kernel is a
// partition of unity).
void buildDensityGrid(const DensityGrid &g, const std::vector<double> &x,
                      const std::vector<double> &y, std::size_t nMov,
                      const std::vector<double> &area, std::vector<std::vector<double>> &partial) {
    const DeterministicChunks ck(nMov);
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, ck.nChunks, ck.nChunks),
        [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t c = r.begin(); c != r.end(); ++c) {
                std::vector<double> &p = partial[c];
                for (std::size_t i = ck.begin(c); i < ck.end(c); ++i) {
                    const double cx = std::clamp(x[i], g.x0, g.x0 + g.dx * g.nbx);
                    const double cy = std::clamp(y[i], g.y0, g.y0 + g.dy * g.nby);
                    const double xf = (cx - g.x0) / g.dx;
                    const double yf = (cy - g.y0) / g.dy;
                    const int ix0 = static_cast<int>(std::floor(xf));
                    const int iy0 = static_cast<int>(std::floor(yf));
                    const double a = area[i];
                    for (int dy = -1; dy <= 1; ++dy) {
                        const int iy = iy0 + dy;
                        if (iy < 0 || iy >= g.nby)
                            continue;
                        const double wy = densityBell(yf - static_cast<double>(iy));
                        if (wy == 0.0)
                            continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int ix = ix0 + dx;
                            if (ix < 0 || ix >= g.nbx)
                                continue;
                            const double w = densityBell(xf - static_cast<double>(ix)) * wy;
                            if (w == 0.0)
                                continue;
                            p[g.idx(ix, iy)] += a * w;
                        }
                    }
                }
            }
        },
        tbb::simple_partitioner{});
}

// Fraction of movable cell area in bins that exceed their capacity.
double densityOverflow(const DensityGrid &g, double totalArea) {
    const double cap = g.dx * g.dy;
    const double sum =
        deterministicReduce(g.occ.size(), [&](std::size_t b0, std::size_t e0, double &acc) {
            for (std::size_t k = b0; k < e0; ++k) {
                // Bins that are (nearly) full of blockage have no spare room, so
                // area stacked on top of them is overflow just as much as area
                // stacked on an empty bin.
                acc += std::max(g.occ[k] - g.cap[k], 0.0);
            }
        });
    return sum / std::max(totalArea, 1e-300);
}

// SimPL-style projection spreading. Cells are ordered by their current bin
// (then by position within the bin); walking that order, the accumulated cell
// area is mapped onto the bin grid at a uniform density `targetDens`, so each
// cell gets a target point in an area-balanced "designed" layout. Pulling cells
// toward these targets drains over-packed bins into empty ones.
struct SpreadCell {
    double area;
    int row;
    int col;
    double x;
    std::size_t idx;
};
void projectionSpread(const DensityGrid &g, const std::vector<double> &x,
                      const std::vector<double> &y, std::size_t nMov,
                      const std::vector<double> &area, double targetDens,
                      const std::vector<double> &occ, std::vector<double> &projX,
                      std::vector<double> &projY, bool gated = true,
                      const constraintMgr *constraints = nullptr) {
    // With gating, only cells sitting in over-full bins drain; cells in bins at
    // or below the uniform capacity keep their current (wirelength-friendly)
    // positions.  Without gating (refinement phase), every cell is pulled
    // toward the equi-area target so the wirelength solve cannot re-collapse
    // the carpet into the corner blob.
    // With region constraints the equi-area walk is done per pool: cells
    // fenced into a region are laid out over that region's own bins, and the
    // remaining cells over the bins no region covers. This keeps every target
    // inside its fence, which the single global walk cannot do.
    constexpr double kOverflowTau = 0.15;
    std::vector<char> overfull(g.occ.size(), 0);
    if (gated) {
        for (std::size_t k = 0; k < g.occ.size(); ++k) {
            overfull[k] = (occ[k] > targetDens * (1.0 + kOverflowTau)) ? 1 : 0;
        }
    } else {
        std::fill(overfull.begin(), overfull.end(), 1);
    }

    std::vector<SpreadCell> cells;
    cells.reserve(nMov);
    for (std::size_t i = 0; i < nMov; ++i) {
        const double cx = std::clamp(x[i], g.x0, g.x0 + g.dx * g.nbx - 1e-9);
        const double cy = std::clamp(y[i], g.y0, g.y0 + g.dy * g.nby - 1e-9);
        const int ix = std::min(static_cast<int>((cx - g.x0) / g.dx), g.nbx - 1);
        const int iy = std::min(static_cast<int>((cy - g.y0) / g.dy), g.nby - 1);
        if (overfull[g.idx(ix, iy)]) {
            cells.push_back({area[i], iy, ix, cx, i});
        } else {
            projX[i] = cx;
            projY[i] = cy;
        }
    }
    std::sort(cells.begin(), cells.end(), [](const SpreadCell &a, const SpreadCell &b) {
        if (a.row != b.row)
            return a.row < b.row;
        if (a.col != b.col)
            return a.col < b.col;
        return a.x < b.x;
    });

    // Target layout: lay the draining cells out at a uniform density over the
    // area that is actually available. A prefix sum of the per-bin capacity does
    // this: bins that are full of macro or reserved by a fence have (almost) no
    // capacity, contribute nothing to the prefix, and are stepped over, so no
    // cell is ever aimed at blocked space.
    std::vector<double> prefix(g.occ.size() + 1, 0.0);
    for (std::size_t k = 0; k < g.occ.size(); ++k) {
        prefix[k + 1] = prefix[k] + g.cap[k];
    }
    const double usable = prefix.back();
    if (!(usable > 0.0) || cells.empty()) {
        for (const SpreadCell &c : cells) {
            projX[c.idx] = std::clamp(x[c.idx], g.x0, g.x0 + g.dx * g.nbx - 1e-9);
            projY[c.idx] = std::clamp(y[c.idx], g.y0, g.y0 + g.dy * g.nby - 1e-9);
        }
    } else {
        double cellArea = 0.0;
        for (const SpreadCell &c : cells) {
            cellArea += c.area;
        }
        // Uniform density over the usable area, then invert to get, for each
        // accumulated cell area, the position along that area.
        const double scale = usable / std::max(cellArea, 1e-300);
        double cum = 0.0;
        for (const SpreadCell &c : cells) {
            const double pos = std::min(cum * scale, usable - 1e-9);
            const std::size_t k = static_cast<std::size_t>(
                std::upper_bound(prefix.begin(), prefix.end(), pos) - prefix.begin() - 1);
            const std::size_t kk = std::min(k, g.occ.size() - 1);
            const double within = g.cap[kk] > 0.0 ? (pos - prefix[kk]) / g.cap[kk] : 0.5;
            projX[c.idx] = g.x0 + (static_cast<double>(kk % static_cast<std::size_t>(g.nbx)) +
                                   within) * g.dx;
            projY[c.idx] = g.y0 + (static_cast<double>(kk / static_cast<std::size_t>(g.nbx)) +
                                   ((within * 7.0) - std::floor(within * 7.0))) * g.dy;
            cum += c.area;
        }
    }

    // Make the targets fence-aware. The equi-area map above is deliberately
    // left untouched: it is the one that actually drains a placement, and giving
    // each fence its own walk (an earlier attempt) made the whole population
    // drift towards one die edge instead of spreading. Instead every target is
    // made legal here -- a fenced cell's target is pulled into its own region,
    // an unconstrained cell's target is pushed out of every region -- so the
    // drain now aims at legal spots. The caller's per-iteration clamp and
    // push-out then hold the positions themselves.
    if (constraints != nullptr) {
        const double dieMaxX = g.x0 + g.dx * static_cast<double>(g.nbx);
        const double dieMaxY = g.y0 + g.dy * static_cast<double>(g.nby);
        for (std::size_t i = 0; i < nMov; ++i) {
            const int region = g.regionOfMov.empty() ? constraintMgr::kNoRegion
                                                      : g.regionOfMov[i];
            if (region != constraintMgr::kNoRegion) {
                constraints->clampToRegion(region, projX[i], projY[i]);
            } else {
                constraints->pushOutOfRegions(projX[i], projY[i], g.x0, g.y0, dieMaxX, dieMaxY);
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Placement-frame snapshot capture for the SVG/HTML plotter.
QuadraticPlacer::QuadraticPlacer(PlacementDB &database) : db(database) {}

PlacerResult QuadraticPlacer::place(int maxIter, double tol, const std::string &plotDir,
                                   const constraintMgr *constraints) {
    using clock = std::chrono::steady_clock;
    PlacerResult result;
    const Graph &g = db.getGraph();
    const std::size_t nv = g.getNumVertices();

    std::vector<std::size_t> varOfVertex(nv, kNoIndex);  // cell vertex id -> variable id
    std::vector<std::size_t> movableVertex;              // variable id -> cell vertex id
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type == VertexType::Cell && !vert.isTerminal && !vert.isFixed) {
            varOfVertex[v] = movableVertex.size();
            movableVertex.push_back(v);
        }
    }
    const std::size_t numMovable = movableVertex.size();
    result.numMovable = numMovable;

    // Per-net classification (parallel): count distinct cells, mark big nets.
    std::vector<std::size_t> netDegree(nv, 0);
    std::vector<std::size_t> netStarVar(nv, kNoIndex);
    std::vector<double> netWeight(nv, 1.0);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nv),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t v = r.begin(); v != r.end(); ++v) {
                              const Vertex &vert = g.getVertex(v);
                              if (vert.type != VertexType::Net)
                                  continue;
                              netWeight[v] = vert.weight;
                              netDegree[v] = netCellIds(g, vert).size();
                          }
                      });

    // Assign contiguous star-node variables (serial numbering).
    std::size_t starBase = numMovable;
    std::size_t numStars = 0;
    for (std::size_t v = 0; v < nv; ++v) {
        if (g.getVertex(v).type == VertexType::Net && netDegree[v] > kStarThreshold) {
            netStarVar[v] = starBase + numStars;
            ++numStars;
        }
    }
    result.numStars = numStars;
    const std::size_t n = numMovable + numStars;
    if (n == 0) {
        return result;
    }

    // ------------------------------------------------------------------
    // Matrix assembly.  Variables: [0, numMovable) movable cells,
    // [numMovable, n) star nodes.
    // ------------------------------------------------------------------
    // Chunked assembly so both the diagonal sums and the CSR storage order are
    // bitwise fixed. Chunk c records its partial diagonal in chunkDiag[c] and its
    // (row, col, val) off-diagonals in chunkTri[c]; the chunks are combined in
    // index order and the triples stably sorted by row, preserving the fixed
    // within-row emission order. (The previous relaxed atomics let the summation
    // order follow thread scheduling, which fed tiny floating-point differences
    // into the CG solve and made identical runs diverge into different
    // placements.)
    struct Triple {
        std::size_t row;
        std::size_t col;
        double val;
    };
    const DeterministicChunks ckNet(nv);
    std::vector<std::vector<double>> chunkDiag(ckNet.nChunks, std::vector<double>(n, 0.0));
    std::vector<std::vector<Triple>> chunkTri(ckNet.nChunks);
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, ckNet.nChunks, ckNet.nChunks),
        [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t c = r.begin(); c != r.end(); ++c) {
                std::vector<double> &diag = chunkDiag[c];
                std::vector<Triple> &tri = chunkTri[c];
                for (std::size_t v = ckNet.begin(c); v < ckNet.end(c); ++v) {
                    const Vertex &vert = g.getVertex(v);
                    if (vert.type != VertexType::Net || netDegree[v] == 0) {
                        continue;
                    }
                    const std::vector<std::size_t> ids = netCellIds(g, vert);
                    const std::size_t k = ids.size();
                    const double wNet = netWeight[v];

                    if (k > kStarThreshold) {
                        // Star model: every cell connects to the net's star node.
                        const std::size_t sv = netStarVar[v];
                        const double a = wNet / static_cast<double>(k);
                        for (std::size_t cid : ids) {
                            const std::size_t uc = varOfVertex[cid];
                            diag[sv] += a;
                            if (uc != kNoIndex) {
                                diag[uc] += a;
                                tri.push_back(Triple{uc, sv, -a});
                                tri.push_back(Triple{sv, uc, -a});
                            }
                        }
                    } else {
                        // Clique model: weight between any two distinct cells.
                        const double w = wNet * 2.0 / static_cast<double>(k * (k - 1));
                        for (std::size_t a2 = 0; a2 < k; ++a2) {
                            const std::size_t ua = varOfVertex[ids[a2]];
                            if (ua == kNoIndex)
                                continue;
                            for (std::size_t b2 = a2 + 1; b2 < k; ++b2) {
                                const std::size_t ub = varOfVertex[ids[b2]];
                                if (ub == kNoIndex)
                                    continue;
                                diag[ua] += w;
                                diag[ub] += w;
                                tri.push_back(Triple{ua, ub, -w});
                                tri.push_back(Triple{ub, ua, -w});
                            }
                            // Connections to fixed cells contribute only to the
                            // diagonal.
                            for (std::size_t b2 = 0; b2 < k; ++b2) {
                                if (varOfVertex[ids[b2]] == kNoIndex) {
                                    diag[ua] += w;
                                }
                            }
                        }
                    }
                }
            }
        },
        tbb::simple_partitioner{});

    // Combine the per-chunk diagonals in chunk order.
    CsrMatrix m;
    m.n = n;
    m.diag.assign(n, kReg);
    for (std::size_t c = 0; c < ckNet.nChunks; ++c) {
        for (std::size_t i = 0; i < n; ++i) {
            m.diag[i] += chunkDiag[c][i];
        }
    }

    // Concatenate triples in chunk order, stable-sort by row (preserving the
    // fixed emission order within each row) and lay them into CSR.
    std::size_t total = 0;
    for (std::size_t c = 0; c < ckNet.nChunks; ++c) {
        total += chunkTri[c].size();
    }
    std::vector<Triple> all;
    all.reserve(total);
    for (std::size_t c = 0; c < ckNet.nChunks; ++c) {
        all.insert(all.end(), chunkTri[c].begin(), chunkTri[c].end());
    }
    std::stable_sort(all.begin(), all.end(), [](const Triple &a, const Triple &b) {
        return a.row < b.row;
    });
    m.rowPtr.assign(n + 1, 0);
    for (const Triple &tri : all) {
        ++m.rowPtr[tri.row + 1];
    }
    for (std::size_t i = 0; i < n; ++i) {
        m.rowPtr[i + 1] += m.rowPtr[i];
    }
    m.col.resize(total);
    m.val.resize(total);
    std::vector<std::size_t> cursor(n);
    for (std::size_t i = 0; i < n; ++i) {
        cursor[i] = m.rowPtr[i];
    }
    for (const Triple &tri : all) {
        const std::size_t pos = cursor[tri.row]++;
        m.col[pos] = tri.col;
        m.val[pos] = tri.val;
    }

    // Right-hand sides: fixed-cell connections feed b.
    const auto rhsPass = [&](const std::vector<double> &coordFix, std::vector<double> &b) {
        b.assign(n, 0.0);
        const DeterministicChunks ck(nv);
        std::vector<std::vector<double>> chunkB(ck.nChunks, std::vector<double>(n, 0.0));
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, ck.nChunks, ck.nChunks),
            [&](const tbb::blocked_range<std::size_t> &r) {
                for (std::size_t c = r.begin(); c != r.end(); ++c) {
                    std::vector<double> &bacc = chunkB[c];
                    for (std::size_t v = ck.begin(c); v < ck.end(c); ++v) {
                        const Vertex &vert = g.getVertex(v);
                        if (vert.type != VertexType::Net || netDegree[v] == 0)
                            continue;
                        const std::vector<std::size_t> ids = netCellIds(g, vert);
                        const std::size_t k = ids.size();
                        const double wNet = netWeight[v];
                        if (k > kStarThreshold) {
                            const std::size_t sv = netStarVar[v];
                            const double a = wNet / static_cast<double>(k);
                            for (std::size_t cid : ids) {
                                if (varOfVertex[cid] == kNoIndex) {
                                    bacc[sv] += a * coordFix[cid];
                                }
                            }
                        } else {
                            const double w = wNet * 2.0 / static_cast<double>(k * (k - 1));
                            for (std::size_t a2 = 0; a2 < k; ++a2) {
                                const std::size_t ua = varOfVertex[ids[a2]];
                                if (ua == kNoIndex)
                                    continue;
                                for (std::size_t b2 = 0; b2 < k; ++b2) {
                                    const std::size_t cb = ids[b2];
                                    if (varOfVertex[cb] == kNoIndex) {
                                        bacc[ua] += w * coordFix[cb];
                                    }
                                }
                            }
                        }
                    }
                }
            },
            tbb::simple_partitioner{});
        for (std::size_t c = 0; c < ck.nChunks; ++c) {
            for (std::size_t i = 0; i < n; ++i) {
                b[i] += chunkB[c][i];
            }
        }
    };

    // Gather the fixed-vertex coordinates for the right-hand sides.
    std::vector<double> xCoord(n), yCoord(n);
    std::vector<double> coordAllX(nv), coordAllY(nv);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nv),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t v = r.begin(); v != r.end(); ++v) {
                              coordAllX[v] = g.getVertex(v).x;
                              coordAllY[v] = g.getVertex(v).y;
                          }
                      });
    std::vector<double> rhsX, rhsY;
    auto t0 = clock::now();
    rhsPass(coordAllX, rhsX);
    rhsPass(coordAllY, rhsY);
    auto t1 = clock::now();

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              const std::size_t cid = movableVertex[i];
                              xCoord[i] = g.getVertex(cid).x;
                              yCoord[i] = g.getVertex(cid).y;
                          }
                      });

    // ------------------------------------------------------------------
    // Global placement: SimPL-style projection spreading.  Two phases:
    //  phase 0 (outer < kSpreadInIters): pure projection spreading — each
    //      iteration assigns every cell an area-balanced target bin (a gated
    //      sorted scan/equi-area fill of the die, draining cells out of
    //      over-full bins) and nudges cells toward it with a capped walk;
    //  phase 1: the wirelength solve (CG on the fixed connectivity system)
    //      is re-enabled, with the projection target coupled into the RHS
    //      (kProjMu * diag_i times the signed distance) instead of an
    //      unrestricted nudge.
    // ------------------------------------------------------------------
    const bool plot = !plotDir.empty();
    const std::size_t numStepsTotal =
        static_cast<std::size_t>(std::max(maxIter, 1));  // outer-iteration budget
    const BBox dieBox = fixedCellBBox(g);                // die region from fixed cells
    std::vector<std::string> frames;                     // frame file names
    std::vector<std::pair<std::size_t, double>> curve;   // (step, hpwl)
    std::vector<float> allX, allY;                       // per-vertex, float
    std::vector<double> curveResid;                      // density overflow per step
    std::vector<double> curveFencedOut;   // fenced cells outside their own region, per step
    std::vector<double> curveStrangerIn;  // unfenced cells inside some region, per step
    std::size_t worstFencedOut = 0;      // worst value seen in any single iteration
    std::size_t worstStrangerIn = 0;

    const auto packAll = [&](std::vector<float> &out, const std::vector<float> &mov, bool isX) {
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, nv), [&](const tbb::blocked_range<std::size_t> &r2) {
                for (std::size_t v = r2.begin(); v != r2.end(); ++v) {
                    const Vertex &vert = g.getVertex(v);
                    if (vert.type == VertexType::Cell && !vert.isTerminal && !vert.isFixed) {
                        out[v] = mov[varOfVertex[v]];
                    } else {
                        out[v] = static_cast<float>(isX ? vert.x : vert.y);
                    }
                }
            });
    };

    // Per-movable cell area (for the density field) and extents (needed to move
    // a cell clear of a blockage without leaving its own box inside it).
    std::vector<double> cellExtentX(numMovable, 0.0);
    std::vector<double> cellExtentY(numMovable, 0.0);
    std::vector<double> areaMov(numMovable);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              const Vertex &vert = g.getVertex(movableVertex[i]);
                              areaMov[i] = std::max(vert.width, 1.0) * std::max(vert.height, 1.0);
                              cellExtentX[i] = std::max(vert.width, 0.0);
                              cellExtentY[i] = std::max(vert.height, 0.0);
                          }
                      });
    const double totalArea =
        deterministicReduce(numMovable, [&](std::size_t b0, std::size_t e0, double &acc) {
            for (std::size_t i = b0; i < e0; ++i) {
                acc += areaMov[i];
            }
        });

    // Working solution: movable cells [0, numMovable) plus star nodes. The
    // movable cells are seeded at the centre of the die (the supplied .pl
    // files are degenerate — every cell at the origin), stars start at the
    // origin. The projection-spreading phase then drains the centre blob
    // outward over the whole die.
    // Per-movable placement region (kNoRegion when unconstrained).
    std::vector<int> regionOfMov(numMovable, constraintMgr::kNoRegion);
    const bool useRegions = (constraints != nullptr && constraints->hasConstraints());
    if (useRegions) {
        for (std::size_t i = 0; i < numMovable; ++i) {
            regionOfMov[i] = g.getVertex(movableVertex[i]).regionId;
        }
    }

    const double seedX = 0.5 * (dieBox[0] + dieBox[2]);
    const double seedY = 0.5 * (dieBox[1] + dieBox[3]);
    std::vector<double> xSol(n), ySol(n);

    // Seed placement.
    //
    // Unconstrained designs keep the historical behaviour: every cell starts at
    // the die centre and the projection phase drains the blob outward.
    //
    // With fences that seeding is wrong in two ways, so cells are instead laid
    // out per pool at the start:
    //   * a cell seeded inside someone else's fence is illegal, and the fence
    //     may well straddle the die centre, so all unconstrained cells would
    //     begin in violation and spend the run being dragged across a boundary;
    //   * a region can be disconnected, so spreading a group over the region's
    //     bounding box and clamping drops every cell onto the nearest rectangle
    //     edge -- legal, but a degenerate pile on a single corner.
    // So each region is filled equi-area across its own rectangles, and the
    // remaining cells are spread equi-area over the die area that no fence
    // touches. Seeding serially: the equi-area walk is a running sum.
    if (useRegions) {
        const DensityGrid sg = makeDensityGrid(dieBox, numMovable);
        const double binW = sg.dx;
        const double binH = sg.dy;

        // The spread phase is what drains the placement: it works by collapsing
        // every cell onto one point and letting the projection walk them out to
        // an equi-area layout, which is what takes an unfenced design from ~1.0
        // overflow to ~0.001. Seeding pre-spread instead makes the wirelength
        // pull dominate from the first iteration and overflow climbs, so the
        // unconstrained cells still start as a blob -- but on a point that no
        // fence covers, since the die centre is often inside one.
        double blobX = seedX;
        double blobY = seedY;
        if (constraints->insideAnyRegionPublic(blobX, blobY)) {
            // Largest bin that no fence rectangle touches, else the die centre.
            const DensityGrid sg = makeDensityGrid(dieBox, numMovable);
            double bestArea = -1.0;
            for (std::size_t iy = 0; iy < static_cast<std::size_t>(sg.nby); ++iy) {
                for (std::size_t ix = 0; ix < static_cast<std::size_t>(sg.nbx); ++ix) {
                    const double bx0 = sg.x0 + static_cast<double>(ix) * sg.dx;
                    const double by0 = sg.y0 + static_cast<double>(iy) * sg.dy;
                    const double cx = bx0 + 0.5 * sg.dx;
                    const double cy = by0 + 0.5 * sg.dy;
                    if (constraints->insideAnyRegionPublic(cx, cy)) {
                        continue;
                    }
                    // Prefer a bin far from every fence so the blob has room.
                    double nearest = std::numeric_limits<double>::max();
                    for (std::size_t r = 0; r < constraints->numRegions(); ++r) {
                        for (const Rect &rc : constraints->region(static_cast<int>(r))->rects) {
                            const double dx = std::max({rc.lo.x - cx, 0.0, cx - rc.hi.x});
                            const double dy = std::max({rc.lo.y - cy, 0.0, cy - rc.hi.y});
                            nearest = std::min(nearest, std::max(dx, dy));
                        }
                    }
                    if (nearest > bestArea) {
                        bestArea = nearest;
                        blobX = cx;
                        blobY = cy;
                    }
                }
            }
        }

        // Movable indices per region, in vertex order for determinism.
        std::vector<std::vector<std::uint32_t>> cellsOfRegion(constraints->numRegions());
        double freeCellArea = 0.0;
        for (std::size_t i = 0; i < numMovable; ++i) {
            if (regionOfMov[i] == constraintMgr::kNoRegion) {
                freeCellArea += areaMov[i];
            } else {
                cellsOfRegion[static_cast<std::size_t>(regionOfMov[i])].push_back(
                    static_cast<std::uint32_t>(i));
            }
        }

        // Unconstrained cells all start on the blob point chosen above.
        for (std::size_t i = 0; i < numMovable; ++i) {
            if (regionOfMov[i] == constraintMgr::kNoRegion) {
                xSol[i] = blobX;
                ySol[i] = blobY;
            }
        }

        // Each region's own cells equi-area across that region's rectangles.
        for (std::size_t ri = 0; ri < cellsOfRegion.size(); ++ri) {
            const Region *reg = constraints->region(static_cast<int>(ri));
            const std::vector<std::uint32_t> &cells = cellsOfRegion[ri];
            if (reg == nullptr || cells.empty()) {
                continue;
            }
            std::vector<double> cum;
            cum.reserve(reg->rects.size());
            double running = 0.0;
            for (const Rect &rc : reg->rects) {
                running += rc.area();
                cum.push_back(running);
            }
            const double total = std::max(running, 1e-300);
            double acc = 0.0;
            for (std::uint32_t idx : cells) {
                const std::size_t i = idx;
                const double centre = acc + 0.5 * areaMov[i];
                acc += areaMov[i];
                // Rectangle holding this cell's share of the region.
                std::size_t r = 0;
                while (r + 1 < cum.size() && cum[r] < centre) {
                    ++r;
                }
                const Rect &rc = reg->rects[r];
                const double base = (r == 0) ? 0.0 : cum[r - 1];
                const double u = std::clamp((centre - base) / std::max(cum[r] - base, 1e-300), 0.0, 1.0);
                const double v = std::fmod(centre * 0.6180339887498949, 1.0);
                xSol[i] = rc.lo.x + u * (rc.hi.x - rc.lo.x);
                ySol[i] = rc.lo.y + v * (rc.hi.y - rc.lo.y);
            }
        }
    } else {
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i != r.end(); ++i) {
                                  xSol[i] = seedX;
                                  ySol[i] = seedY;
                              }
                          });
    }

    // HPWL of the actual seed placement (center blob): per-vertex coordinates
    // for the metric, pads at their fixed corners.
    std::vector<double> seedAllX(nv), seedAllY(nv);
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, nv), [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t v = r.begin(); v != r.end(); ++v) {
                const Vertex &vert = g.getVertex(v);
                const bool movable =
                    vert.type == VertexType::Cell && !vert.isTerminal && !vert.isFixed;
                seedAllX[v] = movable ? seedX : vert.x;
                seedAllY[v] = movable ? seedY : vert.y;
            }
        });
    result.hpwlInitial = hpwl(g, seedAllX, seedAllY);

    DensityGrid dg = makeDensityGrid(dieBox, numMovable);
    // The pool map only depends on the grid, so it is built once.
    if (useRegions) {
        buildRegionPools(dg, *constraints);
        dg.regionOfMov = regionOfMov;  // consulted when making targets legal
    }
    // Fixed macros, terminals and fences all shrink what is left to place into.
    applyBlockage(dg, g, useRegions ? constraints : nullptr);

    // Fixed cells are hard blockages: the wirelength solve knows nothing about
    // them, so without an explicit correction the solve happily parks cells on
    // top of a macro (measured: half of all movable area on the four blocks of
    // mgc_des_perf_a). Collect their rectangles, and index them per bin so the
    // per-iteration check only ever looks at the few rectangles near the cell.
    std::vector<Rect> macroRects;
    for (std::size_t v = 0; v < g.getNumVertices(); ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || (!vert.isFixed && !vert.isTerminal)) {
            continue;
        }
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        macroRects.push_back(Rect{Point{vert.x, vert.y},
                                  Point{vert.x + vert.width, vert.y + vert.height}});
    }
    std::vector<std::vector<std::uint32_t>> macrosInBin(dg.occ.size());
    for (std::size_t mi = 0; mi < macroRects.size(); ++mi) {
        const Rect &rc = macroRects[mi];
        const double rx0 = std::max(rc.lo.x, dg.x0);
        const double ry0 = std::max(rc.lo.y, dg.y0);
        const double rx1 = std::min(rc.hi.x, dg.x0 + dg.dx * dg.nbx);
        const double ry1 = std::min(rc.hi.y, dg.y0 + dg.dy * dg.nby);
        if (!(rx1 > rx0) || !(ry1 > ry0)) {
            continue;
        }
        const int ix0 = std::clamp(static_cast<int>((rx0 - dg.x0) / dg.dx), 0, dg.nbx - 1);
        const int iy0 = std::clamp(static_cast<int>((ry0 - dg.y0) / dg.dy), 0, dg.nby - 1);
        const int ix1 = std::clamp(static_cast<int>((rx1 - dg.x0) / dg.dx), 0, dg.nbx - 1);
        const int iy1 = std::clamp(static_cast<int>((ry1 - dg.y0) / dg.dy), 0, dg.nby - 1);
        for (int iy = iy0; iy <= iy1; ++iy) {
            for (int ix = ix0; ix <= ix1; ++ix) {
                macrosInBin[dg.idx(ix, iy)].push_back(static_cast<std::uint32_t>(mi));
            }
        }
    }



    // Mean occupancy per bin: the target a perfectly-uniform spread approaches.
    const double targetDens = totalArea / static_cast<double>(dg.nbx * dg.nby);
    std::vector<double> projX(numMovable), projY(numMovable);
    double overflow = 1.0;

    // Rebuild the occupancy grid from the current movable coordinates (read
    // from xSol/ySol, indices below numMovable). The splat is accumulated into
    // fixed per-chunk partials and combined in chunk order, so it is bitwise
    // reproducible iteration to iteration.
    const std::size_t occChunks = DeterministicChunks(numMovable).nChunks;
    std::vector<std::vector<double>> occPartial(occChunks, std::vector<double>(dg.occ.size(), 0.0));
    const auto refreshDensity = [&]() {
        for (std::vector<double> &part : occPartial) {
            std::fill(part.begin(), part.end(), 0.0);
        }
        buildDensityGrid(dg, xSol, ySol, numMovable, areaMov, occPartial);
        for (std::size_t k = 0; k < dg.occ.size(); ++k) {
            double s = 0.0;
            for (std::size_t c = 0; c < occChunks; ++c) {
                s += occPartial[c][k];
            }
            dg.occ[k] = s;
        }
        refreshDensityGradient(dg);
    };

    std::size_t frameIdx = 0;
    const double baseHpwl = result.hpwlInitial;
    const auto emitFrame = [&](std::size_t step, double hpwl, double resid,
                               const std::string &note) {
        std::ostringstream name;
        name << "frame_step_" << std::setw(3) << std::setfill('0') << frameIdx++ << ".svg";
        const std::string path = plotDir + "/" + name.str();
        writeFrameSvg(path, g, allX, allY, dieBox, step, 1 + numStepsTotal, hpwl, baseHpwl, resid,
                      note, constraints);
        frames.push_back(name.str());
        curve.push_back({step, hpwl});
        curveResid.push_back(resid);
    };

    if (plot) {
        ensureDir(plotDir);
        allX.resize(nv);
        allY.resize(nv);
    }

    // Initial frame: the die-center seed and its density field.
    refreshDensity();
    result.densityOverflowInitial = densityOverflow(dg, totalArea);
    if (plot) {
        std::vector<float> xmov(numMovable), ymov(numMovable);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                          [&](const tbb::blocked_range<std::size_t> &r2) {
                              for (std::size_t i = r2.begin(); i != r2.end(); ++i) {
                                  xmov[i] = static_cast<float>(xSol[i]);
                                  ymov[i] = static_cast<float>(ySol[i]);
                              }
                          });
        packAll(allX, xmov, true);
        packAll(allY, ymov, false);
        emitFrame(0, result.hpwlInitial, result.densityOverflowInitial,
                  "initial placement (die-center seed)");
    }

    // ------------------------------------------------------------------
    // Outer (global-placement) loop: WL solve + projection spreading.
    std::vector<double> bx(n), by(n);
    // Density-weighted matrix used by the refinement solves: the diagonal of
    // movable rows is inflated by kProjMu * diag, so the projection targets
    // enter the SYSTEM (not just the right-hand side) and cells cannot snap
    // back to the collapsed wirelength optimum after the pre-spread phase.
    CsrMatrix mSpread = m;
    for (std::size_t i = 0; i < numMovable; ++i) {
        mSpread.diag[i] *= (1.0 + kProjMu);
    }
    // Per-iteration fence checker. A placement is legal only if, at *every*
    // iteration, each cell assigned to a region sits inside it and no
    // unassigned cell sits inside any region -- not merely at the end. The
    // unassigned test is the expensive one (a point may lie in any rectangle of
    // any region), so it is gated on the bin map: buildRegionPools puts every
    // bin that overlaps a fence either into that region's pool or into the
    // blocked set, so a cell in a plain free-pool bin cannot be inside a fence
    // and needs no rectangle tests at all.
    struct FenceCounts {
        std::uint64_t fencedOut = 0;
        std::uint64_t strangerIn = 0;
    };
    const auto countFenceViolations = [&]() {
        FenceCounts counts;
        if (!useRegions) {
            return counts;
        }
        return tbb::parallel_reduce(
            tbb::blocked_range<std::size_t>(0, numMovable), FenceCounts{},
            [&](const tbb::blocked_range<std::size_t> &r, FenceCounts acc) {
                FenceCounts local = acc;
                for (std::size_t i = r.begin(); i != r.end(); ++i) {
                    const int region = regionOfMov[i];
                    if (region != constraintMgr::kNoRegion) {
                        if (!constraints->contains(region, xSol[i], ySol[i])) {
                            ++local.fencedOut;
                        }
                        continue;
                    }
                    const double cx =
                        std::clamp(xSol[i], dg.x0, dg.x0 + dg.dx * dg.nbx - 1e-9);
                    const double cy =
                        std::clamp(ySol[i], dg.y0, dg.y0 + dg.dy * dg.nby - 1e-9);
                    const int ix = std::min(static_cast<int>((cx - dg.x0) / dg.dx), dg.nbx - 1);
                    const int iy = std::min(static_cast<int>((cy - dg.y0) / dg.dy), dg.nby - 1);
                    if (dg.binPool[dg.idx(ix, iy)] == kFreePool) {
                        continue;  // bin is clear of every fence
                    }
                    if (constraints->insideAnyRegionPublic(xSol[i], ySol[i])) {
                        ++local.strangerIn;
                    }
                }
                return local;
            },
            [](FenceCounts a, FenceCounts b) {
                a.fencedOut += b.fencedOut;
                a.strangerIn += b.strangerIn;
                return a;
            });
    };

    std::size_t outer = 0;
    auto t2 = clock::now();
    // Phase 0 (warm-up): the movable cells are seeded as a collapsed blob at
    // the die center (the degenerate .pl seeds every cell at the origin, so the
    // input position is not a state any global placer can refine).  Run a
    // cheap pure-projection pre-spread that carpets the die at uniform density
    // with no wirelength solve at all.
    const std::size_t spreadInIters =
        static_cast<std::size_t>(static_cast<double>(numStepsTotal) * kSpreadPhaseFraction);
    const double maxStep = kMaxStepBins * std::min(dg.dx, dg.dy);
    for (; outer < numStepsTotal; ++outer) {
        // 1) Occupancy from the current positions; drain over-full bins only.
        refreshDensity();
        overflow = densityOverflow(dg, totalArea);
        ktlog.trace("outer {}: density overflow {:.6e}", outer, overflow);
        projectionSpread(dg, xSol, ySol, numMovable, areaMov, targetDens, dg.occ, projX, projY,
                         /*gated=*/true, useRegions ? constraints : nullptr);

        if (outer >= spreadInIters) {
            // Phase 1 (refine): hold the carpet with an un-gated equi-area pull
            //   coupled into the system, then solve warm-started with CG.  The
            //   pull keeps cells spread while the solve lowers wiring cost.
            projectionSpread(dg, xSol, ySol, numMovable, areaMov, targetDens, dg.occ, projX, projY,
                             /*gated=*/false, useRegions ? constraints : nullptr);

            // Geometric ramp: gentle at the start of refinement, dominant by
            // the last iteration.
            const std::size_t refineIters = std::max<std::size_t>(numStepsTotal - spreadInIters, 1);
            const double refineFrac =
                std::clamp(static_cast<double>(outer - spreadInIters) /
                               static_cast<double>(refineIters - 1),
                           0.0, 1.0);
            const double densityWeight =
                kDensityGradStart *
                std::pow(kDensityGradEnd / kDensityGradStart, refineFrac);
            ktlog.trace("outer {}: density weight {:.4g}", outer, densityWeight);

            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, n),
                [&](const tbb::blocked_range<std::size_t> &r) {
                    for (std::size_t i = r.begin(); i != r.end(); ++i) {
                        // Density force: step downhill in utilization, scaled
                        // by the cell's own diagonal so a well-connected cell
                        // feels it in proportion to its wiring. The negative sign
                        // is what makes the solve itself drain crowded bins.
                        double densX = 0.0;
                        double densY = 0.0;
                        if (i < numMovable) {
                            const double cx = std::clamp(xSol[i], dg.x0,
                                                         dg.x0 + dg.dx * dg.nbx - 1e-9);
                            const double cy = std::clamp(ySol[i], dg.y0,
                                                         dg.y0 + dg.dy * dg.nby - 1e-9);
                            const double fx = (cx - dg.x0) / dg.dx;
                            const double fy = (cy - dg.y0) / dg.dy;
                            const int ix = std::min(static_cast<int>(fx), dg.nbx - 1);
                            const int iy = std::min(static_cast<int>(fy), dg.nby - 1);
                            const int ix1 = std::min(ix + 1, dg.nbx - 1);
                            const int iy1 = std::min(iy + 1, dg.nby - 1);
                            const double wx = fx - static_cast<double>(ix);
                            const double wy = fy - static_cast<double>(iy);
                            const std::size_t k00 = dg.idx(ix, iy);
                            const std::size_t k10 = dg.idx(ix1, iy);
                            const std::size_t k01 = dg.idx(ix, iy1);
                            const std::size_t k11 = dg.idx(ix1, iy1);
                            const auto bilerp = [&](const std::vector<double> &f) {
                                return (1.0 - wx) * (1.0 - wy) * f[k00] + wx * (1.0 - wy) * f[k10] +
                                       (1.0 - wx) * wy * f[k01] + wx * wy * f[k11];
                            };
                            densX = bilerp(dg.gradX);
                            densY = bilerp(dg.gradY);
                        }
                        bx[i] = rhsX[i] +
                                (i < numMovable ? kProjMu * m.diag[i] * (projX[i] - xSol[i]) -
                                                      densityWeight * m.diag[i] * densX
                                                : 0.0);
                        by[i] = rhsY[i] +
                                (i < numMovable ? kProjMu * m.diag[i] * (projY[i] - ySol[i]) -
                                                      densityWeight * m.diag[i] * densY
                                                : 0.0);
                    }
                });
            std::size_t it1 = 0, it2 = 0;
            double r1 = 0.0, r2 = 0.0;
            // Trust region. The carpet the pre-spread phase builds is far from
            // the wirelength optimum, so the first refinement solve starts with
            // an enormous gradient; if CG has not converged in its inner budget
            // the step it returns is not a descent direction, and the die clamp
            // then catches the resulting explosion by pinning cells to the die
            // edge -- which is exactly how a whole population ends up stacked in
            // one corner on top of a macro. Capping how far any cell may move per
            // iteration keeps the step sane; the solve simply takes more
            // iterations to get there.
            const std::vector<double> prevX = xSol;
            const std::vector<double> prevY = ySol;
            conjugateGradient(mSpread, bx, xSol, kInnerMaxIter, kInnerTol, it1, r1);
            conjugateGradient(mSpread, by, ySol, kInnerMaxIter, kInnerTol, it2, r2);
            const double maxMove = kMaxMoveBins * std::min(dg.dx, dg.dy);
            tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                              [&](const tbb::blocked_range<std::size_t> &r) {
                                  for (std::size_t i = r.begin(); i != r.end(); ++i) {
                                      xSol[i] = prevX[i] +
                                                std::clamp(xSol[i] - prevX[i], -maxMove, maxMove);
                                      ySol[i] = prevY[i] +
                                                std::clamp(ySol[i] - prevY[i], -maxMove, maxMove);
                                  }
                              });
        }

        // 3) Direct spread: walk each draining cell toward its projection
        //    target by at most a fraction of a bin width per iteration.  The
        //    capped displacement (on top of the coupled force) guarantees the
        //    outward drift survives the next solve, so over-full bins steadily
        //    drain into empty die area while net clusters stay coherent.
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, numMovable),
            [&](const tbb::blocked_range<std::size_t> &r) {
                for (std::size_t i = r.begin(); i != r.end(); ++i) {
                    const double sx = std::clamp(projX[i] - xSol[i], -maxStep, maxStep);
                    const double sy = std::clamp(projY[i] - ySol[i], -maxStep, maxStep);
                    xSol[i] += sx;
                    ySol[i] += sy;
                    // Fixed macros are hard blockages, for fenced and unfenced
                    // cells alike. The wirelength solve has no term for them, so
                    // without this the solve parks cells on top of a blockage (it
                    // did: half of all movable area in mgc_des_perf_a). Step the
                    // cell just clear of whichever blockage it is on, taking the
                    // side that needs the least movement. Only the few blockages
                    // near the cell's own bin are tested.
                    if (!macrosInBin.empty() && cellExtentX[i] > 0.0 && cellExtentY[i] > 0.0) {
                        const double cw = cellExtentX[i];
                        const double ch = cellExtentY[i];
                        const double cx =
                            std::clamp(xSol[i], dg.x0, dg.x0 + dg.dx * dg.nbx - 1e-9);
                        const double cy =
                            std::clamp(ySol[i], dg.y0, dg.y0 + dg.dy * dg.nby - 1e-9);
                        const int ix = std::min(static_cast<int>((cx - dg.x0) / dg.dx), dg.nbx - 1);
                        const int iy = std::min(static_cast<int>((cy - dg.y0) / dg.dy), dg.nby - 1);
                        for (std::uint32_t mi : macrosInBin[dg.idx(ix, iy)]) {
                            const Rect &rc = macroRects[mi];
                            if (std::min(xSol[i] + cw, rc.hi.x) <= std::max(xSol[i], rc.lo.x) ||
                                std::min(ySol[i] + ch, rc.hi.y) <= std::max(ySol[i], rc.lo.y)) {
                                continue;  // clear of this blockage
                            }
                            const double eps = 1e-6 * std::max(dg.dx, dg.dy);
                            const double candX[2] = {rc.lo.x - cw - eps, rc.hi.x + eps};
                            const double candY[2] = {rc.lo.y - ch - eps, rc.hi.y + eps};
                            int pick = 0;
                            double bestMove = std::abs(candX[0] - xSol[i]);
                            const double moveY[2] = {std::abs(candY[0] - ySol[i]),
                                                      std::abs(candY[1] - ySol[i])};
                            if (std::abs(candX[1] - xSol[i]) < bestMove) {
                                bestMove = std::abs(candX[1] - xSol[i]);
                                pick = 1;
                            }
                            if (moveY[0] < bestMove) {
                                bestMove = moveY[0];
                                pick = 2;
                            }
                            if (moveY[1] < bestMove) {
                                pick = 3;
                            }
                            if (pick < 2) {
                                xSol[i] = candX[pick];
                            } else {
                                ySol[i] = candY[pick - 2];
                            }
                        }
                    }

                    // Legality last: leaving a blockage can drop a cell into a
                    // fence, and a fence is a hard rule, so the fence clamp and
                    // push-out run after the blockage step and get the final say.
                    if (constraints != nullptr) {
                        if (regionOfMov[i] != constraintMgr::kNoRegion) {
                            constraints->clampToRegion(regionOfMov[i], xSol[i], ySol[i]);
                        } else {
                            constraints->pushOutOfRegions(xSol[i], ySol[i], dieBox[0], dieBox[1],
                                                           dieBox[2], dieBox[3]);
                        }
                    }
                    xSol[i] = std::clamp(xSol[i], dieBox[0], dieBox[2]);
                    ySol[i] = std::clamp(ySol[i], dieBox[1], dieBox[3]);
                }
            });

        // 4) Fence check: the fixup above must have made this iteration legal.
        {
            const FenceCounts counts = countFenceViolations();
            curveFencedOut.push_back(static_cast<double>(counts.fencedOut));
            curveStrangerIn.push_back(static_cast<double>(counts.strangerIn));
            worstFencedOut = std::max(worstFencedOut, static_cast<std::size_t>(counts.fencedOut));
            worstStrangerIn =
                std::max(worstStrangerIn, static_cast<std::size_t>(counts.strangerIn));
            ktlog.trace("outer {}: fenced_out={} stranger_in={} overflow={:.6e}", outer,
                        counts.fencedOut, counts.strangerIn, overflow);
            if (counts.fencedOut != 0 || counts.strangerIn != 0) {
                ktlog.echo("iter {}: ILLEGAL placement -- {} fenced cell(s) outside, "
                           "{} unfenced cell(s) inside a fence",
                           outer, counts.fencedOut, counts.strangerIn);
            }
        }

        // 5) HPWL evaluation and plotting of the diluted placement.
        if (plot) {
            std::vector<float> xmov(numMovable), ymov(numMovable);
            tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                              [&](const tbb::blocked_range<std::size_t> &r2) {
                                  for (std::size_t i = r2.begin(); i != r2.end(); ++i) {
                                      xmov[i] = static_cast<float>(xSol[i]);
                                      ymov[i] = static_cast<float>(ySol[i]);
                                  }
                              });
            packAll(allX, xmov, true);
            packAll(allY, ymov, false);
            const double hpwlNow = hpwlF(g, allX, allY);
            ktlog.trace("outer step {}: hpwl={:.6e} overflow={:.6e}", outer + 1, hpwlNow, overflow);
            emitFrame(outer + 1, hpwlNow, overflow,
                      "outer iter " + std::to_string(outer + 1) + " (WL + density)");
        }

        // 6) Convergence: density overflow below target, only after the wirelength
        //    refinement has had at least half its iteration budget (so the
        //    solve has time to reduce wiring inside the spread placement).
        if (outer + 1 > spreadInIters &&
            outer + 1 >= spreadInIters + (numStepsTotal - spreadInIters) / 2 && overflow <= tol) {
            break;
        }
    }
    auto t3 = clock::now();

    if (plot) {
        writeHpwlCurve(plotDir + "/hpwl.csv", plotDir + "/hpwl.svg", curve, &curveResid);
        // Per-iteration fence legality, one row per outer step, so a violation
        // can be located in time rather than only noticed in the final result.
        {
            std::ofstream csv(plotDir + "/constraints.csv");
            csv << "step,fenced_out,stranger_in\n";
            for (std::size_t k = 0; k < curveFencedOut.size(); ++k) {
                csv << (k + 1) << ',' << static_cast<std::uint64_t>(curveFencedOut[k]) << ','
                    << static_cast<std::uint64_t>(curveStrangerIn[k]) << '\n';
            }
        }
        writeGallery(plotDir, frames, "hpwl.csv");
        ktlog.echo("Wrote {} frames + HPWL/overflow curve to {} (see index.html)", frames.size(),
                   plotDir);
    }

    // Copy the final solution back into xCoord/yCoord for the database.
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              xCoord[i] = xSol[i];
                              yCoord[i] = ySol[i];
                          }
                      });

    // Write results back into the database (serial; independent per cell).
    for (std::size_t i = 0; i < numMovable; ++i) {
        db.setCellPosition(movableVertex[i], xCoord[i], yCoord[i]);
    }

    // Final HPWL from updated positions.
    std::vector<double> finalX(nv), finalY(nv);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nv),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t v = r.begin(); v != r.end(); ++v) {
                              finalX[v] = g.getVertex(v).x;
                              finalY[v] = g.getVertex(v).y;
                          }
                      });
    auto t4 = clock::now();
    result.hpwlFinal = hpwl(g, finalX, finalY);
    auto t5 = clock::now();

    if (useRegions) {
        ktlog.echo("Fence legality over {} iteration(s): worst fenced_out={}, "
                   "worst stranger_in={}, final fenced_out={}, final stranger_in={}",
                   curveFencedOut.size(), worstFencedOut, worstStrangerIn,
                   curveFencedOut.empty() ? 0 : static_cast<std::size_t>(curveFencedOut.back()),
                   curveStrangerIn.empty() ? 0
                                          : static_cast<std::size_t>(curveStrangerIn.back()));
    }

    result.numIterations = outer;
    result.finalResidual = overflow;
    result.densityOverflowFinal = overflow;
    result.buildSeconds = std::chrono::duration<double>(t1 - t0).count();
    result.solveSeconds = std::chrono::duration<double>(t3 - t2).count();
    result.hpwlSeconds = std::chrono::duration<double>(t5 - t4).count();

    return result;
}

}  // namespace ktplace
