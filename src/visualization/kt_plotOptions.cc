// @file kt_plotOptions.cc
// Plot settings, from defaults and the environment

#include "visualization/kt_plotOptions.h"

#include "visualization/visualizer_options.h"

#include <cstdlib>
#include <string>

namespace ktplace {

namespace {}  // namespace

PlotOptions PlotOptions::fromOptions() {
    PlotOptions out;
    const VisualizerOptions opts = VisualizerOptions::load();
    out.animate = opts.anim;
    out.frameBudget = opts.animMaxFrames;
    out.frameZoom = opts.animZoom;
    out.finalZoom = opts.finalZoom;
    out.writePpm = opts.finalPpm;
    out.gifByteCap = opts.animMaxBytes;
    out.frameDelayCs = opts.animDelayCs;
    out.blendFrames = opts.animBlend;
    out.finalHold = opts.animFinalHold;
    out.traceEvery = opts.animTraceEvery;
    out.cgFrameEvery = opts.animCgEvery;
    out.densityMaps = opts.animDensityMaps;
    opts.report();
    return out;
}

}  // namespace ktplace
