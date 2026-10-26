#include "geometry/kt_geometry.h"

#include <algorithm>

namespace ktplace {

double Interval::getCenter() const noexcept {
    return 0.5 * (start + end);
}

bool Interval::contains(double v, double tolerance) const noexcept {
    return v >= start - tolerance && v <= end + tolerance;
}

double Interval::getClamped(double v) const noexcept {
    return std::clamp(v, start, end);
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
    return Point(0.5 * (lowerLeft.x + upperRight.x), 0.5 * (lowerLeft.y + upperRight.y));
}

Interval BBox::getX() const noexcept {
    return Interval(lowerLeft.x, upperRight.x);
}

Interval BBox::getY() const noexcept {
    return Interval(lowerLeft.y, upperRight.y);
}

bool BBox::isEmpty() const noexcept {
    return lowerLeft.x > upperRight.x || lowerLeft.y > upperRight.y;
}

bool BBox::contains(const Point &p) const noexcept {
    return p.x >= lowerLeft.x && p.x <= upperRight.x && p.y >= lowerLeft.y && p.y <= upperRight.y;
}

bool BBox::isOverlapping(const BBox &o) const noexcept {
    return lowerLeft.x < o.upperRight.x && o.lowerLeft.x < upperRight.x &&
           lowerLeft.y < o.upperRight.y && o.lowerLeft.y < upperRight.y;
}

BBox BBox::getIntersection(const BBox &o) const noexcept {
    return BBox(
        Point(std::max(lowerLeft.x, o.lowerLeft.x), std::max(lowerLeft.y, o.lowerLeft.y)),
        Point(std::min(upperRight.x, o.upperRight.x), std::min(upperRight.y, o.upperRight.y)));
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
