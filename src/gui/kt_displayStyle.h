#pragma once

#include <QColor>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ktplace::gui {

enum class DisplayLayer : std::uint8_t {
    Die,
    Rows,
    Macros,
    MovableCells,
    Pads,
    Fences,
    Selection,
    MarkedNets,
    RoutingDemand,
};

inline constexpr std::array displayLayers{
    DisplayLayer::Die,          DisplayLayer::Rows,       DisplayLayer::Macros,
    DisplayLayer::MovableCells, DisplayLayer::Pads,       DisplayLayer::Fences,
    DisplayLayer::Selection,    DisplayLayer::MarkedNets, DisplayLayer::RoutingDemand,
};

struct LayerLook {
    bool visible = true;
    QColor color;

    friend bool operator==(const LayerLook &, const LayerLook &) = default;
};

class DisplayStyle {
public:
    DisplayStyle();

    [[nodiscard]] const LayerLook &operator[](DisplayLayer layer) const;
    [[nodiscard]] LayerLook &operator[](DisplayLayer layer);
    [[nodiscard]] bool shows(DisplayLayer layer) const;

    [[nodiscard]] static std::string_view title(DisplayLayer layer);
    [[nodiscard]] static std::string_view key(DisplayLayer layer);
    [[nodiscard]] static std::optional<DisplayLayer> fromKey(std::string_view key);

    friend bool operator==(const DisplayStyle &, const DisplayStyle &) = default;

private:
    std::array<LayerLook, displayLayers.size()> looks;
};

}  // namespace ktplace::gui
