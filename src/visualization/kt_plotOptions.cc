// @file kt_plotOptions.cc
// Plot settings, from defaults and the run's visualizer options

#include "visualization/kt_plotOptions.h"

#include "visualization/visualizer_options.h"

#include <string>

namespace ktplace {

PlotOptions PlotOptions::fromOptions(const std::string &plotDir) {
    PlotOptions out;
    const auto opts = loadOptions<VisualizerOptions>();
    out.dir = opts.plot ? plotDir : std::string();
    out.animate = opts.anim;
    out.frameBudget = opts.animMaxFrames;
    out.frameZoom = opts.animZoom;
    out.finalZoom = opts.finalZoom;
    out.blendFrames = opts.animBlend;
    out.keepSnapshots = opts.keepSnapshots;
    out.svgFrames = opts.svgFrames;
    reportOptions(opts);
    return out;
}

}  // namespace ktplace
