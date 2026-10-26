#pragma once

#include <limits>

namespace ktplace {

struct Point {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const Point &, const Point &) = default;
};

struct Interval {
    double lo = 0.0;
    double hi = 0.0;

    [[nodiscard]] double getCenter() const noexcept;
    [[nodiscard]] bool contains(double v, double tolerance = 0.0) const noexcept;
    [[nodiscard]] double getClamped(double v) const noexcept;
};

struct BBox {
    Point lowerLeft;
    Point upperRight;

    [[nodiscard]] static constexpr BBox getEmpty() noexcept {
        constexpr double inf = std::numeric_limits<double>::infinity();
        return {.lowerLeft = {.x = inf, .y = inf}, .upperRight = {.x = -inf, .y = -inf}};
    }
    [[nodiscard]] static constexpr BBox of(double x, double y, double width,
                                           double height) noexcept {
        return {.lowerLeft = {.x = x, .y = y}, .upperRight = {.x = x + width, .y = y + height}};
    }

    [[nodiscard]] double getWidth() const noexcept;
    [[nodiscard]] double getHeight() const noexcept;
    [[nodiscard]] double getArea() const noexcept;
    [[nodiscard]] double getHalfPerimeter() const noexcept;
    [[nodiscard]] Point getCenter() const noexcept;
    [[nodiscard]] Interval getX() const noexcept;
    [[nodiscard]] Interval getY() const noexcept;
    [[nodiscard]] bool isEmpty() const noexcept;

    [[nodiscard]] bool contains(const Point &p) const noexcept;
    [[nodiscard]] bool isOverlapping(const BBox &o) const noexcept;
    [[nodiscard]] BBox getIntersection(const BBox &o) const noexcept;

    void extend(double x, double y) noexcept;
    void extend(const Point &p) noexcept;
    void extend(const BBox &o) noexcept;

    friend bool operator==(const BBox &, const BBox &) = default;
};

}  // namespace ktplace
