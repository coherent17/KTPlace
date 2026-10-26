#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ktplace {
class ktDM;
}

namespace ktplace::gui {

enum class CellKind : std::uint8_t { Movable, Macro, Pad, MovableMacro };

struct Bounds {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    void include(double x, double y);
    void include(const Bounds &other);
    [[nodiscard]] double getHalfPerimeter() const;
};

struct LayoutGeometry {
    struct Fence {
        std::string name;
        std::vector<Bounds> rects;
    };

    struct Row {
        float y = 0.0F;
        float height = 0.0F;
        float xlo = 0.0F;
        float xhi = 0.0F;
    };

    std::vector<std::string> name;
    std::vector<float> width;
    std::vector<float> height;
    std::vector<CellKind> kind;
    std::vector<std::int32_t> fenceOf;
    std::vector<std::string> netName;
    std::vector<Fence> fences;
    std::vector<std::uint32_t> cellNetStart;
    std::vector<std::uint32_t> cellNet;
    std::vector<std::uint32_t> netPinStart;
    std::vector<std::uint32_t> pinCell;
    std::vector<float> pinDx;
    std::vector<float> pinDy;
    std::vector<Row> rows;
    std::array<double, 4> die{};
    float typicalWidth = 1.0F;

    [[nodiscard]] static std::shared_ptr<const LayoutGeometry> of(const ktDM &db);
};

struct LayoutFrame {
    std::shared_ptr<const LayoutGeometry> geometry;
    std::vector<float> x;
    std::vector<float> y;
    double hpwl = 0.0;
    std::string stage;

    [[nodiscard]] static LayoutFrame getCommitted(std::shared_ptr<const LayoutGeometry> geometry,
                                                  const ktDM &db, std::string stage);
    [[nodiscard]] bool isDrawable() const;
    [[nodiscard]] Bounds getCellBounds(std::uint32_t cell) const;
    [[nodiscard]] std::optional<Bounds> getCellsBounds(std::span<const std::uint32_t> cells) const;
    [[nodiscard]] std::optional<Bounds> getNetBounds(std::uint32_t net) const;
    [[nodiscard]] std::optional<Bounds> getNetsBounds(std::span<const std::uint32_t> nets) const;
};

}  // namespace ktplace::gui
