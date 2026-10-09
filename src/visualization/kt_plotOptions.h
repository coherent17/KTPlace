// @file kt_plotOptions.h
// What a run should draw, and where

#pragma once

#include <cstddef>
#include <string>

namespace ktplace {

// Everything about a run's pictures, in one place.
//
// The command line only decides whether a run draws at all. The settings below
// are the defaults, and fromEnvironment() lets the environment override them, so
// tuning a run's pictures does not mean inventing a flag for each one and
// threading it through the flow.
struct PlotOptions {
    // Root directory for everything drawn. Empty means draw nothing.
    std::string dir;

    bool animate = true;                 // record frames and assemble the GIF
    std::size_t frameBudget = 1200;      // frames offered before the animator thins
    double frameZoom = 2.0;              // frame scale against the 768x768 frame size
    std::size_t gifByteCap = 96u << 20;  // 96 MB opens in a browser
    int frameDelayCs = 12;               // 120 ms per frame
    int blendFrames = 3;                 // in-between frames per placement
    double finalZoom = 8.0;              // scale of the single high-resolution still
    std::size_t finalHold = 8;           // extra frames the finished placement is held
    bool writePpm = false;               // also write the lossless raster, 113 MB at 8x

    // Cadence of the per-iteration frames, in solver iterations.
    std::size_t traceEvery = 5;
    std::size_t cgFrameEvery = 1;
    bool densityMaps = false;

    // The defaults, with the environment applied over them.
    [[nodiscard]] static PlotOptions fromEnvironment();

    [[nodiscard]] bool enabled() const {
        return !dir.empty();
    }

    // A stage's own directory under the root, or empty when drawing is off.
    [[nodiscard]] std::string sub(const char *stage) const {
        return enabled() ? dir + "/" + stage : std::string();
    }
};

}  // namespace ktplace
