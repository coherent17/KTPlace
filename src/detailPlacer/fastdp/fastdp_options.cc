// @file fastdp_options.cc
// Reading the detailed placer's options from the option store

#include "detailPlacer/fastdp/fastdp_options.h"

namespace ktplace {

namespace {

const Option<std::size_t> kWindow{"fastdp", "window", 0};
const Option<bool> kNoMedian{"fastdp", "no_median", false};

}  // namespace

FastdpOptions FastdpOptions::load() {
    FastdpOptions out;
    out.window = kWindow.value();
    out.noMedian = kNoMedian.value();
    out.rows = {resolve(kWindow), resolve(kNoMedian)};
    return out;
}

void FastdpOptions::report() const {
    reportOptions("fastdp", rows);
}

}  // namespace ktplace
