// @file simpl_options.cc
// Reading SimPL's options from the option store

#include "placer/simpl/simpl_options.h"

namespace ktplace {

namespace {

const Option<int> kGrid{"simpl", "grid", 0};
const Option<double> kCgTol{"simpl", "cg_tol", 1e-4};
const Option<std::size_t> kInitIters{"simpl", "init_iters", 15};
const Option<std::size_t> kIters{"simpl", "iters", 100};
const Option<std::size_t> kSeed{"simpl", "seed", 20240607};
const Option<bool> kFences{"simpl", "fences", true};
const Option<double> kDensity{"simpl", "density", 1.0};
const Option<double> kAlphaBase{"simpl", "alpha_base", 0.01};

}  // namespace

SimplOptions SimplOptions::load() {
    SimplOptions out;
    out.grid = kGrid.value();
    out.cgTol = kCgTol.value();
    out.initIters = kInitIters.value();
    out.iters = kIters.value();
    out.seed = kSeed.value();
    out.fences = kFences.value();
    out.density = kDensity.value();
    out.alphaBase = kAlphaBase.value();
    out.rows = {resolve(kGrid), resolve(kCgTol),  resolve(kInitIters), resolve(kIters),
                resolve(kSeed), resolve(kFences), resolve(kDensity),   resolve(kAlphaBase)};
    return out;
}

void SimplOptions::report() const {
    reportOptions("simpl", rows);
}

}  // namespace ktplace
