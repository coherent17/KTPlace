// @file visualizer_options.h
// What a run draws, and how, and where it came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <string>
#include <vector>

namespace ktplace {

// Every option the drawing side reads:
//
//     add_option visualizer anim 0
//     add_option visualizer anim_zoom 2
//
// Set with `add_option visualizer <option> <value>`; the defaults are below.
struct VisualizerOptions {
    // Record frames and assemble the GIF. Off means nothing is drawn.
    bool anim = true;

    // Frames offered before the animator thins to fit its byte cap.
    std::size_t animMaxFrames = 1200;

    // Frame scale against the 768x768 frame size.
    double animZoom = 2.0;

    // Scale of the single high-resolution still: 8 turns a 768 frame into 6144.
    double finalZoom = 8.0;

    // Also write the lossless raster, which is 113 MB at 8x.
    bool finalPpm = false;

    // The GIF's size cap, 96 MB: what a browser still opens.
    std::size_t animMaxBytes = std::size_t{96} << 20;
    // Centiseconds per frame, and in-between frames per recorded placement.
    int animDelayCs = 12;
    int animBlend = 3;
    // Extra frames the finished placement is held for at the end.
    std::size_t animFinalHold = 8;
    // The stages' own cadence: SimPL frames every N outer iterations and every N
    // conjugate-gradient steps (0 = none), and whether it draws density maps.
    std::size_t animTraceEvery = 1;
    std::size_t animCgEvery = 0;
    bool animDensityMaps = true;

    [[nodiscard]] static VisualizerOptions load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
