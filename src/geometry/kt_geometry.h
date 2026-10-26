#pragma once

#include <compare>
#include <limits>

namespace ktplace {

struct Point {
    double x = 0.0;
    double y = 0.0;

    constexpr Point() noexcept = default;
    constexpr explicit Point(double xValue, double yValue) noexcept : x(xValue), y(yValue) {}

    friend bool operator==(const Point &, const Point &) = default;
};

struct Interval {
    double start = 0.0;
    double end = 0.0;

    constexpr Interval() noexcept = default;
    constexpr explicit Interval(double startValue, double endValue) noexcept
        : start(startValue), end(endValue) {}

    [[nodiscard]] double getLength() const noexcept;
    [[nodiscard]] double getCenter() const noexcept;
    [[nodiscard]] bool contains(double v, double tolerance = 0.0) const noexcept;
    [[nodiscard]] double getClamped(double v) const noexcept;

    friend constexpr auto operator<=>(const Interval &, const Interval &) = default;
};

struct BBox {
    Point lowerLeft;
    Point upperRight;

    constexpr BBox() noexcept = default;
    constexpr explicit BBox(const Point &lowerLeftCorner, const Point &upperRightCorner) noexcept
        : lowerLeft(lowerLeftCorner), upperRight(upperRightCorner) {}

    [[nodiscard]] static constexpr BBox getEmpty() noexcept {
        constexpr double inf = std::numeric_limits<double>::infinity();
        return BBox(Point(inf, inf), Point(-inf, -inf));
    }
    [[nodiscard]] static constexpr BBox of(double x, double y, double width,
                                           double height) noexcept {
        return BBox(Point(x, y), Point(x + width, y + height));
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
