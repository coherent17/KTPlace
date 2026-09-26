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
#include "util/kt_log.h"
#include "visualization/kt_plotter.h"

#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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
// Pull strength of the projection targets inside the coupled RHS.  Deliberately
// weak: it only shifts the WL equilibrium a little toward the drain target, so
// the wirelength solve keeps the clusters coherent while the (read-only) bias
// lets the capped nudge accumulate into a steady outward drift.
constexpr double kProjMu = 2.0;
// Outer iterations reserved for the pure-projection pre-spread phase (see
// PLACER loop).  Covers the collapsed die-center seed before refining.
constexpr std::size_t kSpreadInIters = 200;
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
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, a.size()), 0.0,
        [&](const tbb::blocked_range<std::size_t> &r, double acc) {
            for (std::size_t i = r.begin(); i != r.end(); ++i) {
                acc += a[i] * b[i];
            }
            return acc;
        },
        [](double x, double y) {
            return x + y;
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
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, nv), 0.0,
        [&](const tbb::blocked_range<std::size_t> &r, double acc) {
            for (std::size_t v = r.begin(); v != r.end(); ++v) {
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
            return acc;
        },
        [](double a, double b2) {
            return a + b2;
        });
}

// hpwl() over float per-vertex coordinate arrays (used by the frame plotter).
double hpwlF(const Graph &g, const std::vector<float> &x, const std::vector<float> &y) {
    const std::size_t nv = g.getNumVertices();
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, nv), 0.0,
        [&](const tbb::blocked_range<std::size_t> &r, double acc) {
            for (std::size_t v = r.begin(); v != r.end(); ++v) {
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
            return acc;
        },
        [](double a, double b2) {
            return a + b2;
        });
}

// ---------------------------------------------------------------------------
// Density grid used for spreading and for the overflow metric: a uniform bin
// grid over the die. Cells are spread with a SimPL-style projection: each
// movable cell is assigned an area-balanced target bin (sorted scanne /area
// fill), then pulled toward it; over-packed regions drain into empty ones.
// ---------------------------------------------------------------------------
struct DensityGrid {
    int nbx = 8;
    int nby = 8;
    double x0 = 0.0;
    double y0 = 0.0;
    double dx = 1.0;
    double dy = 1.0;
    std::vector<double> occ;  // accumulated cell area per bin

    std::size_t idx(int ix, int iy) const {
        return static_cast<std::size_t>(iy) * static_cast<std::size_t>(nbx) +
               static_cast<std::size_t>(ix);
    }
};

// Uniform bin grid covering the die bounding box.
DensityGrid makeDensityGrid(const BBox &die) {
    const double w = std::max(die[2] - die[0], 1.0);
    const double h = std::max(die[3] - die[1], 1.0);
    constexpr int kBaseBins = 64;
    const int nbx = std::max(4, kBaseBins);
    const int nby = std::max(4, static_cast<int>(std::lround(kBaseBins * h / std::max(w, 1.0))));
    DensityGrid g;
    g.nbx = nbx;
    g.nby = nby;
    g.x0 = die[0];
    g.y0 = die[1];
    g.dx = w / static_cast<double>(nbx);
    g.dy = h / static_cast<double>(nby);
    g.occ.assign(static_cast<std::size_t>(nbx) * nby, 0.0);
    return g;
}

// Accumulate movable cell area into the grid with bilinear (smooth) spreading.
// x/y hold the full solve vector; only the first nMov entries participate.
void buildDensityGrid(const DensityGrid &g, const std::vector<double> &x,
                      const std::vector<double> &y, std::size_t nMov,
                      const std::vector<double> &area, std::vector<std::atomic<double>> &occAcc) {
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, nMov), [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t i = r.begin(); i != r.end(); ++i) {
                const double cx = std::clamp(x[i], g.x0, g.x0 + g.dx * g.nbx);
                const double cy = std::clamp(y[i], g.y0, g.y0 + g.dy * g.nby);
                const double xf = (cx - g.x0) / g.dx;
                const double yf = (cy - g.y0) / g.dy;
                int ix = static_cast<int>(std::floor(xf));
                int iy = static_cast<int>(std::floor(yf));
                ix = std::clamp(ix, 0, g.nbx - 1);
                iy = std::clamp(iy, 0, g.nby - 1);
                const double wx = xf - std::floor(xf);
                const double wy = yf - std::floor(yf);
                const double a = area[i];
                occAcc[g.idx(ix, iy)].fetch_add(a * (1.0 - wx) * (1.0 - wy),
                                                std::memory_order_relaxed);
                if (ix + 1 < g.nbx) {
                    occAcc[g.idx(ix + 1, iy)].fetch_add(a * wx * (1.0 - wy),
                                                        std::memory_order_relaxed);
                }
                if (iy + 1 < g.nby) {
                    occAcc[g.idx(ix, iy + 1)].fetch_add(a * (1.0 - wx) * wy,
                                                        std::memory_order_relaxed);
                }
                if (ix + 1 < g.nbx && iy + 1 < g.nby) {
                    occAcc[g.idx(ix + 1, iy + 1)].fetch_add(a * wx * wy, std::memory_order_relaxed);
                }
            }
        });
}

// Fraction of movable cell area in bins that exceed their capacity.
double densityOverflow(const DensityGrid &g, double totalArea) {
    const double cap = g.dx * g.dy;
    const double sum = tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, g.occ.size()), 0.0,
        [&](const tbb::blocked_range<std::size_t> &r, double acc) {
            for (std::size_t k = r.begin(); k != r.end(); ++k) {
                acc += std::max(g.occ[k] - cap, 0.0);
            }
            return acc;
        },
        [](double a, double b) {
            return a + b;
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
                      std::vector<double> &projY, bool gated = true) {
    // With gating, only cells sitting in over-full bins drain; cells in bins at
    // or below the uniform capacity keep their current (wirelength-friendly)
    // positions.  Without gating (refinement phase), every cell is pulled
    // toward the equi-area target so the wirelength solve cannot re-collapse
    // the carpet into the corner blob.
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

    const double totalBins = static_cast<double>(g.nbx) * static_cast<double>(g.nby);
    const double inv = (targetDens > 1e-30) ? 1.0 / targetDens : 0.0;
    double cum = 0.0;
    for (const SpreadCell &c : cells) {
        double slot = cum * inv;
        if (slot >= totalBins) {
            slot = totalBins - 1e-9;
        }
        const int scol = static_cast<int>(slot) % g.nbx;
        const int srow = static_cast<int>(slot) / g.nbx;
        projX[c.idx] = g.x0 + (static_cast<double>(scol) + 0.5) * g.dx;
        projY[c.idx] = g.y0 + (static_cast<double>(srow) + 0.5) * g.dy;
        cum += c.area;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// Placement-frame snapshot capture for the SVG/HTML plotter.
QuadraticPlacer::QuadraticPlacer(PlacementDB &database) : db(database) {}

PlacerResult QuadraticPlacer::place(int maxIter, double tol, const std::string &plotDir) {
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
    std::vector<std::atomic<std::size_t>> rowCount(n);
    std::vector<std::atomic<double>> diagAcc(n);
    for (std::size_t i = 0; i < n; ++i) {
        rowCount[i].store(0);
        diagAcc[i].store(0.0);
    }

    const auto assemblePass = [&](std::size_t v,
                                  auto emit) {  // emit(uc, vc, wOff) for off-diagonal
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Net || netDegree[v] == 0) {
            return;
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
                diagAcc[sv].fetch_add(a, std::memory_order_relaxed);
                if (uc != kNoIndex) {
                    diagAcc[uc].fetch_add(a, std::memory_order_relaxed);
                    emit(uc, sv, -a);
                    emit(sv, uc, -a);
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
                    diagAcc[ua].fetch_add(w, std::memory_order_relaxed);
                    diagAcc[ub].fetch_add(w, std::memory_order_relaxed);
                    emit(ua, ub, -w);
                    emit(ub, ua, -w);
                }
                // Connections to fixed cells contribute only to the diagonal.
                for (std::size_t b2 = 0; b2 < k; ++b2) {
                    if (varOfVertex[ids[b2]] == kNoIndex) {
                        diagAcc[ua].fetch_add(w, std::memory_order_relaxed);
                    }
                }
            }
        }
    };

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nv),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t v = r.begin(); v != r.end(); ++v) {
                              if (g.getVertex(v).type != VertexType::Net)
                                  continue;
                              assemblePass(v, [&](std::size_t r0, std::size_t, double) {
                                  rowCount[r0].fetch_add(1, std::memory_order_relaxed);
                              });
                          }
                      });

    // CSR row pointers (prefix sum of rowCount).
    CsrMatrix m;
    m.n = n;
    m.rowPtr.assign(n + 1, 0);
    std::size_t total = 0;
    for (std::size_t i = 0; i < n; ++i) {
        m.rowPtr[i] = total;
        total += rowCount[i].load();
    }
    m.rowPtr[n] = total;
    m.col.resize(total);
    m.val.resize(total);

    // Fill pass (parallel): store (row, col, value) at per-row atomic cursors.
    std::vector<std::atomic<std::size_t>> cursor(n);
    for (std::size_t i = 0; i < n; ++i) {
        cursor[i].store(m.rowPtr[i]);
    }
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, nv), [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t v = r.begin(); v != r.end(); ++v) {
                if (g.getVertex(v).type != VertexType::Net)
                    continue;
                assemblePass(v, [&](std::size_t r0, std::size_t c0, double w) {
                    const std::size_t pos = cursor[r0].fetch_add(1, std::memory_order_relaxed);
                    m.col[pos] = c0;
                    m.val[pos] = w;
                });
            }
        });

    // Diagonal + regularization.
    m.diag.assign(n, kReg);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              m.diag[i] += diagAcc[i].load();
                          }
                      });

    // Right-hand sides: fixed-cell connections feed b.
    const auto rhsPass = [&](const std::vector<double> &coordFix, std::vector<double> &b) {
        b.assign(n, 0.0);
        tbb::parallel_for(
            tbb::blocked_range<std::size_t>(0, nv), [&](const tbb::blocked_range<std::size_t> &r) {
                for (std::size_t v = r.begin(); v != r.end(); ++v) {
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
                                b[sv] += a * coordFix[cid];
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
                                    b[ua] += w * coordFix[cb];
                                }
                            }
                        }
                    }
                }
            });
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

    // Per-movable cell area (for the density field).
    std::vector<double> areaMov(numMovable);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              const Vertex &vert = g.getVertex(movableVertex[i]);
                              areaMov[i] = std::max(vert.width, 1.0) * std::max(vert.height, 1.0);
                          }
                      });
    const double totalArea = tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, numMovable), 0.0,
        [&](const tbb::blocked_range<std::size_t> &r, double acc) {
            for (std::size_t i = r.begin(); i != r.end(); ++i) {
                acc += areaMov[i];
            }
            return acc;
        },
        [](double a, double b) {
            return a + b;
        });

    // Working solution: movable cells [0, numMovable) plus star nodes. The
    // movable cells are seeded at the centre of the die (the supplied .pl
    // files are degenerate — every cell at the origin), stars start at the
    // origin. The projection-spreading phase then drains the centre blob
    // outward over the whole die.
    const double seedX = 0.5 * (dieBox[0] + dieBox[2]);
    const double seedY = 0.5 * (dieBox[1] + dieBox[3]);
    std::vector<double> xSol(n), ySol(n);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              xSol[i] = seedX;
                              ySol[i] = seedY;
                          }
                      });

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

    DensityGrid dg = makeDensityGrid(dieBox);
    std::vector<std::atomic<double>> occAcc(dg.occ.size());
    for (std::size_t k = 0; k < occAcc.size(); ++k) {
        occAcc[k].store(0.0, std::memory_order_relaxed);
    }
    // Mean occupancy per bin: the target a perfectly-uniform spread approaches.
    const double targetDens = totalArea / static_cast<double>(dg.nbx * dg.nby);
    std::vector<double> projX(numMovable), projY(numMovable);
    double overflow = 1.0;

    // Rebuild the occupancy grid from the current movable coordinates (read
    // from xSol/ySol, indices below numMovable).
    const auto refreshDensity = [&]() {
        for (std::size_t k = 0; k < occAcc.size(); ++k) {
            occAcc[k].store(0.0, std::memory_order_relaxed);
        }
        buildDensityGrid(dg, xSol, ySol, numMovable, areaMov, occAcc);
        for (std::size_t k = 0; k < dg.occ.size(); ++k) {
            dg.occ[k] = occAcc[k].load(std::memory_order_relaxed);
        }
    };

    std::size_t frameIdx = 0;
    const double baseHpwl = result.hpwlInitial;
    const auto emitFrame = [&](std::size_t step, double hpwl, double resid,
                               const std::string &note) {
        std::ostringstream name;
        name << "frame_step_" << std::setw(3) << std::setfill('0') << frameIdx++ << ".svg";
        const std::string path = plotDir + "/" + name.str();
        writeFrameSvg(path, g, allX, allY, dieBox, step, 1 + numStepsTotal, hpwl, baseHpwl, resid,
                      note);
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
    std::size_t outer = 0;
    auto t2 = clock::now();
    // Phase 0 (warm-up): the movable cells are seeded as a collapsed blob at
    // the die center (the degenerate .pl seeds every cell at the origin, so the
    // input position is not a state any global placer can refine).  Run a
    // cheap pure-projection pre-spread that carpets the die at uniform density
    // with no wirelength solve at all.
    const std::size_t spreadInIters = std::min(kSpreadInIters, numStepsTotal);
    const double maxStep = kMaxStepBins * std::min(dg.dx, dg.dy);
    for (; outer < numStepsTotal; ++outer) {
        // 1) Occupancy from the current positions; drain over-full bins only.
        refreshDensity();
        overflow = densityOverflow(dg, totalArea);
        ktlog.trace("outer {}: density overflow {:.6e}", outer, overflow);
        projectionSpread(dg, xSol, ySol, numMovable, areaMov, targetDens, dg.occ, projX, projY);

        if (outer >= spreadInIters) {
            // Phase 1 (refine): hold the carpet with an un-gated equi-area pull
            //   coupled into the system, then solve warm-started with CG.  The
            //   pull keeps cells spread while the solve lowers wiring cost.
            projectionSpread(dg, xSol, ySol, numMovable, areaMov, targetDens, dg.occ, projX, projY,
                             /*gated=*/false);
            tbb::parallel_for(
                tbb::blocked_range<std::size_t>(0, n),
                [&](const tbb::blocked_range<std::size_t> &r) {
                    for (std::size_t i = r.begin(); i != r.end(); ++i) {
                        bx[i] = rhsX[i] +
                                (i < numMovable ? kProjMu * m.diag[i] * (projX[i] - xSol[i]) : 0.0);
                        by[i] = rhsY[i] +
                                (i < numMovable ? kProjMu * m.diag[i] * (projY[i] - ySol[i]) : 0.0);
                    }
                });
            std::size_t it1 = 0, it2 = 0;
            double r1 = 0.0, r2 = 0.0;
            conjugateGradient(mSpread, bx, xSol, kInnerMaxIter, kInnerTol, it1, r1);
            conjugateGradient(mSpread, by, ySol, kInnerMaxIter, kInnerTol, it2, r2);
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
                    xSol[i] = std::clamp(xSol[i], dieBox[0], dieBox[2]);
                    ySol[i] = std::clamp(ySol[i], dieBox[1], dieBox[3]);
                }
            });

        // 4) HPWL evaluation and plotting of the diluted placement.
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

        // 5) Convergence: density overflow below target, only after the wirelength
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

    result.numIterations = outer;
    result.finalResidual = overflow;
    result.densityOverflowFinal = overflow;
    result.buildSeconds = std::chrono::duration<double>(t1 - t0).count();
    result.solveSeconds = std::chrono::duration<double>(t3 - t2).count();
    result.hpwlSeconds = std::chrono::duration<double>(t5 - t4).count();

    return result;
}

}  // namespace ktplace
