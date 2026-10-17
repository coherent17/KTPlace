#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {
class ktDM;
}

namespace ktplace::gui {

enum class CellKind : std::uint8_t { Movable, Macro, Pad };

struct LayoutGeometry {
    struct Row {
        float y = 0.0F;
        float height = 0.0F;
        float xlo = 0.0F;
        float xhi = 0.0F;
    };

    std::vector<float> width;
    std::vector<float> height;
    std::vector<CellKind> kind;
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

    [[nodiscard]] static LayoutFrame committed(std::shared_ptr<const LayoutGeometry> geometry,
                                               const ktDM &db, std::string stage);
    [[nodiscard]] bool drawable() const;
};

}  // namespace ktplace::gui
