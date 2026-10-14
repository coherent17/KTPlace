// @file multirow_options.cc
// Reading the multi-row legalizer's options from the option store

#include "legalizer/multirow/multirow_options.h"

namespace ktplace {

namespace {

const Option<std::size_t> kFrameEvery{"multirow", "frame_every", 20000};

}  // namespace

MultirowOptions MultirowOptions::load() {
    MultirowOptions out;
    out.frameEvery = kFrameEvery.value();
    out.rows = {resolve(kFrameEvery)};
    return out;
}

void MultirowOptions::report() const {
    reportOptions("multirow", rows);
}

}  // namespace ktplace
