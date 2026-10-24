#include "gui/kt_displayStyle.h"

#include <QColor>
#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace ktplace::gui {

namespace {

struct LayerInfo {
    std::string_view title;
    std::string_view key;
    QRgb color;
    bool visible;
};

constexpr std::array<LayerInfo, displayLayers.size()> layerInfo{{
    {"Die outline", "die", 0x8b97a3, true},
    {"Placement rows", "rows", 0x3b454e, true},
    {"Fixed macros", "macros", 0x7d8a99, true},
    {"Movable cells", "cells", 0x4ab5e5, true},
    {"I/O pads", "pads", 0xe0a030, true},
    {"Fence regions", "fences", 0x9b7fe6, true},
    {"Selection", "selection", 0xff3b30, true},
    {"Connectivity", "nets", 0xff3b30, true},
    {"Routing demand", "demand", 0xff7043, false},
}};

std::size_t indexOf(DisplayLayer layer) {
    return static_cast<std::size_t>(layer);
}

}  // namespace

DisplayStyle::DisplayStyle() {
    for (const DisplayLayer layer : displayLayers) {
        const LayerInfo &info = layerInfo[indexOf(layer)];
        looks[indexOf(layer)] = {.visible = info.visible, .color = QColor::fromRgb(info.color)};
    }
}

const LayerLook &DisplayStyle::operator[](DisplayLayer layer) const {
    return looks[indexOf(layer)];
}

LayerLook &DisplayStyle::operator[](DisplayLayer layer) {
    return looks[indexOf(layer)];
}

bool DisplayStyle::shows(DisplayLayer layer) const {
    return looks[indexOf(layer)].visible;
}

std::string_view DisplayStyle::title(DisplayLayer layer) {
    return layerInfo[indexOf(layer)].title;
}

std::string_view DisplayStyle::key(DisplayLayer layer) {
    return layerInfo[indexOf(layer)].key;
}

std::optional<DisplayLayer> DisplayStyle::fromKey(std::string_view key) {
    for (const DisplayLayer layer : displayLayers) {
        if (layerInfo[indexOf(layer)].key == key) {
            return layer;
        }
    }
    return std::nullopt;
}

}  // namespace ktplace::gui
