#include "geometry/kt_geometry.h"

#include <algorithm>

namespace ktplace {

double Interval::getCenter() const noexcept {
    return 0.5 * (lo + hi);
}

bool Interval::contains(double v, double tolerance) const noexcept {
    return v >= lo - tolerance && v <= hi + tolerance;
}

double BBox::getWidth() const noexcept {
    return upperRight.x - lowerLeft.x;
}

double BBox::getHeight() const noexcept {
    return upperRight.y - lowerLeft.y;
}

double BBox::getArea() const noexcept {
    return getWidth() * getHeight();
}

double BBox::getHalfPerimeter() const noexcept {
    return isEmpty() ? 0.0 : getWidth() + getHeight();
}

Point BBox::getCenter() const noexcept {
    return {.x = 0.5 * (lowerLeft.x + upperRight.x), .y = 0.5 * (lowerLeft.y + upperRight.y)};
}

Interval BBox::getX() const noexcept {
    return {.lo = lowerLeft.x, .hi = upperRight.x};
}

Interval BBox::getY() const noexcept {
    return {.lo = lowerLeft.y, .hi = upperRight.y};
}

bool BBox::isEmpty() const noexcept {
    return lowerLeft.x > upperRight.x || lowerLeft.y > upperRight.y;
}

bool BBox::contains(double x, double y) const noexcept {
    return x >= lowerLeft.x && x <= upperRight.x && y >= lowerLeft.y && y <= upperRight.y;
}

bool BBox::isOverlapping(const BBox &o) const noexcept {
    return lowerLeft.x < o.upperRight.x && o.lowerLeft.x < upperRight.x &&
           lowerLeft.y < o.upperRight.y && o.lowerLeft.y < upperRight.y;
}

BBox BBox::getIntersection(const BBox &o) const noexcept {
    return {.lowerLeft = {.x = std::max(lowerLeft.x, o.lowerLeft.x),
                          .y = std::max(lowerLeft.y, o.lowerLeft.y)},
            .upperRight = {.x = std::min(upperRight.x, o.upperRight.x),
                           .y = std::min(upperRight.y, o.upperRight.y)}};
}

void BBox::extend(double x, double y) noexcept {
    lowerLeft.x = std::min(lowerLeft.x, x);
    lowerLeft.y = std::min(lowerLeft.y, y);
    upperRight.x = std::max(upperRight.x, x);
    upperRight.y = std::max(upperRight.y, y);
}

void BBox::extend(const Point &p) noexcept {
    extend(p.x, p.y);
}

void BBox::extend(const BBox &o) noexcept {
    lowerLeft.x = std::min(lowerLeft.x, o.lowerLeft.x);
    lowerLeft.y = std::min(lowerLeft.y, o.lowerLeft.y);
    upperRight.x = std::max(upperRight.x, o.upperRight.x);
    upperRight.y = std::max(upperRight.y, o.upperRight.y);
}

}  // namespace ktplace
