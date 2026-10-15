// @file ntuplace1_dm.cc
// NTUPlace1's own database

#include "placer/ntuplace1/ntuplace1_dm.h"

namespace ktplace::ntuplace1 {

ntuplace1DM::ntuplace1DM(ktDM &shared) : shared(shared) {}

Ntuplace1Options ntuplace1DM::options() const {
    const Ntuplace1Options out = Ntuplace1Options::load();
    out.report();
    return out;
}

RatioPlaceParams ntuplace1DM::params() const {
    const Ntuplace1Options opts = Ntuplace1Options::load();
    RatioPlaceParams out;
    out.targetLeafCells = opts.leafCells;
    out.maxLevels = opts.maxLevels;
    out.maxRatioRetries = opts.retries;
    out.verbose = opts.verbose;
    return out;
}

ntuplace1Solution ntuplace1DM::solution() const {
    ntuplace1Solution out;
    const Graph &g = shared.getGraph();
    out.xs.resize(g.getNumCells());
    out.ys.resize(g.getNumCells());
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &cell = g.getCell(v);
        out.xs[v] = cell.x;
        out.ys[v] = cell.y;
    }
    return out;
}

ntuplace1DM buildNtuplace1DM(ktDM &shared) {
    return ntuplace1DM(shared);
}

}  // namespace ktplace::ntuplace1
