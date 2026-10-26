// @file visualizer_options.h
// What a run draws, and how

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <tuple>

namespace ktplace {

struct VisualizerOptions {
    static constexpr Component component = Component::Visualizer;

    bool plot = true;
    bool anim = true;
    std::size_t animMaxFrames = 1200;
    double animZoom = 2.0;
    double finalZoom = 8.0;
    int animBlend = 3;
    bool keepSnapshots = false;
    bool svgFrames = false;
    bool congestion = true;
    std::size_t congestionBins = 128;

    static constexpr auto getConfigurableOptions() {
        return std::tuple{
            option("plot", &VisualizerOptions::plot),
            option("anim", &VisualizerOptions::anim),
            option("anim_max_frames", &VisualizerOptions::animMaxFrames),
            option("anim_zoom", &VisualizerOptions::animZoom),
            option("final_zoom", &VisualizerOptions::finalZoom),
            option("anim_blend", &VisualizerOptions::animBlend),
            option("keep_snapshots", &VisualizerOptions::keepSnapshots),
            option("svg_frames", &VisualizerOptions::svgFrames),
            option("congestion", &VisualizerOptions::congestion),
            option("congestion_bins", &VisualizerOptions::congestionBins),
        };
    }
};

}  // namespace ktplace
