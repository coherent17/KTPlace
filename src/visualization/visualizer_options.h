// @file visualizer_options.h
// What a run draws, and how

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>

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

    static void fields(auto &self, auto &&visit) {
        visit("plot", self.plot);
        visit("anim", self.anim);
        visit("anim_max_frames", self.animMaxFrames);
        visit("anim_zoom", self.animZoom);
        visit("final_zoom", self.finalZoom);
        visit("anim_blend", self.animBlend);
        visit("keep_snapshots", self.keepSnapshots);
        visit("svg_frames", self.svgFrames);
    }
};

}  // namespace ktplace
