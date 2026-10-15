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
// Each was a KTPLACE_ANIM_* environment variable. The environment is no longer
// read; a script is the only way to set them.
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

    [[nodiscard]] static VisualizerOptions load();
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
};

}  // namespace ktplace
