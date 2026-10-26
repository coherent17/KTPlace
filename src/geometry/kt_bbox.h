#pragma once

#include "geometry/kt_interval.h"
#include "geometry/kt_point.h"

#include <algorithm>
#include <limits>

namespace ktplace {

struct BBox {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] static constexpr BBox getEmpty() noexcept {
        constexpr double inf = std::numeric_limits<double>::infinity();
        return {inf, inf, -inf, -inf};
    }
    [[nodiscard]] static constexpr BBox of(double x, double y, double width,
                                           double height) noexcept {
        return {x, y, x + width, y + height};
    }

    [[nodiscard]] double getWidth() const noexcept {
        return xhi - xlo;
    }
    [[nodiscard]] double getHeight() const noexcept {
        return yhi - ylo;
    }
    [[nodiscard]] double getArea() const noexcept {
        return getWidth() * getHeight();
    }
    [[nodiscard]] double getHalfPerimeter() const noexcept {
        return isEmpty() ? 0.0 : getWidth() + getHeight();
    }
    [[nodiscard]] Point getCenter() const noexcept {
        return {0.5 * (xlo + xhi), 0.5 * (ylo + yhi)};
    }
    [[nodiscard]] Interval getX() const noexcept {
        return {xlo, xhi};
    }
    [[nodiscard]] Interval getY() const noexcept {
        return {ylo, yhi};
    }
    [[nodiscard]] bool isEmpty() const noexcept {
        return xlo > xhi || ylo > yhi;
    }

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
    [[nodiscard]] bool isOverlapping(const BBox &o) const noexcept {
        return xlo < o.xhi && o.xlo < xhi && ylo < o.yhi && o.ylo < yhi;
    }
    [[nodiscard]] BBox getIntersection(const BBox &o) const noexcept {
        return {std::max(xlo, o.xlo), std::max(ylo, o.ylo), std::min(xhi, o.xhi),
                std::min(yhi, o.yhi)};
    }

    void extend(double x, double y) noexcept {
        xlo = std::min(xlo, x);
        ylo = std::min(ylo, y);
        xhi = std::max(xhi, x);
        yhi = std::max(yhi, y);
    }
    void extend(const BBox &o) noexcept {
        xlo = std::min(xlo, o.xlo);
        ylo = std::min(ylo, o.ylo);
        xhi = std::max(xhi, o.xhi);
        yhi = std::max(yhi, o.yhi);
    }

    friend bool operator==(const BBox &, const BBox &) = default;
};

}  // namespace ktplace
