// @file kt_plotOptions.cc
// Plot settings, from defaults and the run's visualizer options

#include "visualization/kt_plotOptions.h"

#include "visualization/visualizer_options.h"

#include <cstdlib>

namespace ktplace {

namespace {}  // namespace

PlotOptions PlotOptions::fromOptions() {
    PlotOptions out;
    const auto opts = loadOptions<VisualizerOptions>();
    out.animate = opts.anim;
    out.frameBudget = opts.animMaxFrames;
    out.frameZoom = opts.animZoom;
    out.finalZoom = opts.finalZoom;
    out.writePpm = opts.finalPpm;
    out.blendFrames = opts.animBlend;
    out.keepSnapshots = opts.keepSnapshots;
    reportOptions(opts);
    return out;
}

}  // namespace ktplace
