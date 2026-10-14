// @file visualizer_options.cc
// Reading the drawing options from the option store

#include "visualization/visualizer_options.h"

namespace ktplace {

namespace {

const Option<bool> kAnim{"visualizer", "anim", true};
const Option<std::size_t> kAnimMaxFrames{"visualizer", "anim_max_frames", 1200};
const Option<double> kAnimZoom{"visualizer", "anim_zoom", 2.0};
const Option<double> kFinalZoom{"visualizer", "final_zoom", 8.0};
const Option<bool> kFinalPpm{"visualizer", "final_ppm", false};
const Option<std::size_t> kAnimMaxBytes{"visualizer", "anim_max_bytes", std::size_t{96} << 20};
const Option<int> kAnimDelayCs{"visualizer", "anim_delay_cs", 12};
const Option<int> kAnimBlend{"visualizer", "anim_blend", 3};
const Option<std::size_t> kAnimFinalHold{"visualizer", "anim_final_hold", 8};
const Option<std::size_t> kAnimTraceEvery{"visualizer", "anim_trace_every", 1};
const Option<std::size_t> kAnimCgEvery{"visualizer", "anim_cg_every", 0};
const Option<bool> kAnimDensityMaps{"visualizer", "anim_density_maps", true};

}  // namespace

VisualizerOptions VisualizerOptions::load() {
    VisualizerOptions out;
    out.anim = kAnim.value();
    out.animMaxFrames = kAnimMaxFrames.value();
    out.animZoom = kAnimZoom.value();
    out.finalZoom = kFinalZoom.value();
    out.finalPpm = kFinalPpm.value();
    out.animMaxBytes = kAnimMaxBytes.value();
    out.animDelayCs = kAnimDelayCs.value();
    out.animBlend = kAnimBlend.value();
    out.animFinalHold = kAnimFinalHold.value();
    out.animTraceEvery = kAnimTraceEvery.value();
    out.animCgEvery = kAnimCgEvery.value();
    out.animDensityMaps = kAnimDensityMaps.value();
    out.rows = {resolve(kAnim),           resolve(kAnimMaxFrames), resolve(kAnimZoom),
                resolve(kFinalZoom),      resolve(kFinalPpm),      resolve(kAnimMaxBytes),
                resolve(kAnimDelayCs),    resolve(kAnimBlend),     resolve(kAnimFinalHold),
                resolve(kAnimTraceEvery), resolve(kAnimCgEvery),   resolve(kAnimDensityMaps)};
    return out;
}

void VisualizerOptions::report() const {
    reportOptions("visualizer", rows);
}

}  // namespace ktplace
