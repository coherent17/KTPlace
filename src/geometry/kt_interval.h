#pragma once

namespace ktplace {

struct Interval {
    double lo = 0.0;
    double hi = 0.0;

    [[nodiscard]] double getCenter() const noexcept {
        return 0.5 * (lo + hi);
    }
    [[nodiscard]] bool contains(double v, double tolerance = 0.0) const noexcept {
        return v >= lo - tolerance && v <= hi + tolerance;
    }
};

}  // namespace ktplace
