// @file kt_plotOptions.cc
// Plot settings, from defaults and the environment

#include "visualization/kt_plotOptions.h"

#include "util/kt_param.h"

#include <cstdint>
#include <optional>

namespace ktplace {

namespace {

// The registry parses these the way this file always has (EnvParse::
// NumberOrDefault): an unset variable and a nonsense one are the same mistake,
// and the default is the better answer than a zero. Only an override is taken
// from it, so the defaults stay the ones declared in the struct.
template <typename T>
void apply(T &field, const char *name) {
    if (const std::optional<T> v = paramOverride<T>(name)) {
        field = *v;
    }
}

void applyInt(int &field, const char *name) {
    if (const std::optional<std::int64_t> v = paramOverride<std::int64_t>(name)) {
        field = static_cast<int>(*v);
    }
}

}  // namespace

PlotOptions PlotOptions::fromEnvironment() {
    PlotOptions options;

    apply(options.animate, "KTPLACE_ANIM");
    apply(options.frameBudget, "KTPLACE_ANIM_MAX_FRAMES");
    apply(options.gifByteCap, "KTPLACE_ANIM_MAX_BYTES");
    applyInt(options.frameDelayCs, "KTPLACE_ANIM_DELAY_CS");
    applyInt(options.blendFrames, "KTPLACE_ANIM_BLEND");
    apply(options.frameZoom, "KTPLACE_ANIM_ZOOM");
    apply(options.finalZoom, "KTPLACE_FINAL_ZOOM");
    apply(options.writePpm, "KTPLACE_FINAL_PPM");
    apply(options.finalHold, "KTPLACE_ANIM_FINAL_HOLD");
    // Cadence defaults are in the struct. The overrides exist for a long run
    // where you want fewer frames than the default, not because the setting
    // cannot be changed any other way.
    apply(options.traceEvery, "KTPLACE_ANIM_TRACE_EVERY");
    apply(options.cgFrameEvery, "KTPLACE_ANIM_CG_EVERY");
    apply(options.densityMaps, "KTPLACE_ANIM_DENSITY_MAPS");
    return options;
}

}  // namespace ktplace
