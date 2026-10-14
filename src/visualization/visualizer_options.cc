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

}  // namespace

VisualizerOptions VisualizerOptions::load() {
    VisualizerOptions out;
    out.anim = kAnim.value();
    out.animMaxFrames = kAnimMaxFrames.value();
    out.animZoom = kAnimZoom.value();
    out.finalZoom = kFinalZoom.value();
    out.finalPpm = kFinalPpm.value();
    out.rows = {resolve(kAnim), resolve(kAnimMaxFrames), resolve(kAnimZoom), resolve(kFinalZoom),
                resolve(kFinalPpm)};
    return out;
}

void VisualizerOptions::report() const {
    reportOptions("visualizer", rows);
}

}  // namespace ktplace
