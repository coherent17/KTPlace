// @file kt_plotOptions.cc
// Plot settings, from defaults and the environment

#include "visualization/kt_plotOptions.h"

#include <cstdlib>
#include <string>

namespace ktplace {

namespace {

// Read an environment variable as a number, leaving `fallback` when it is unset
// or unparseable. An unset variable and a nonsense one are the same mistake here:
// the default is the better answer than a zero.
double number(const char *name, double fallback) {
    const char *value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    try {
        return std::stod(value);
    } catch (const std::exception &) {
        return fallback;
    }
}

}  // namespace

PlotOptions PlotOptions::fromEnvironment() {
    PlotOptions options;
    const PlotOptions defaults = options;

    options.animate = number("KTPLACE_ANIM", options.animate ? 1.0 : 0.0) != 0.0;
    options.frameBudget = static_cast<std::size_t>(
        number("KTPLACE_ANIM_MAX_FRAMES", static_cast<double>(options.frameBudget)));
    options.gifByteCap = static_cast<std::size_t>(
        number("KTPLACE_ANIM_MAX_BYTES", static_cast<double>(options.gifByteCap)));
    options.frameDelayCs = static_cast<int>(number("KTPLACE_ANIM_DELAY_CS", defaults.frameDelayCs));
    options.blendFrames = static_cast<int>(number("KTPLACE_ANIM_BLEND", defaults.blendFrames));
    options.frameZoom = number("KTPLACE_ANIM_ZOOM", defaults.frameZoom);
    options.finalZoom = number("KTPLACE_FINAL_ZOOM", defaults.finalZoom);
    options.writePpm = number("KTPLACE_FINAL_PPM", defaults.writePpm ? 1.0 : 0.0) != 0.0;
    options.finalHold = static_cast<std::size_t>(
        number("KTPLACE_ANIM_FINAL_HOLD", static_cast<double>(defaults.finalHold)));
    options.traceEvery = static_cast<std::size_t>(
        number("KTPLACE_SIMPL_TRACE_EVERY", static_cast<double>(defaults.traceEvery)));
    options.cgFrameEvery = static_cast<std::size_t>(
        number("KTPLACE_SIMPL_CG_EVERY", static_cast<double>(defaults.cgFrameEvery)));
    options.densityMaps = number("KTPLACE_SIMPL_DENSITY_MAPS", 0.0) != 0.0;
    return options;
}

}  // namespace ktplace
