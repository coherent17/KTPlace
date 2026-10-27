#pragma once

#include "geometry/kt_geometry.h"

#include <cstddef>
#include <vector>

namespace ktplace::kraftwerk2 {

class Potential {
public:
    Potential(const BBox &region, std::size_t bins);

    [[nodiscard]] std::size_t getBins() const noexcept {
        return n;
    }
    [[nodiscard]] const BBox &getRegion() const noexcept {
        return area;
    }
    [[nodiscard]] double getBinWidth() const noexcept {
        return binWidth;
    }
    [[nodiscard]] double getBinHeight() const noexcept {
        return binHeight;
    }

    void solve(const std::vector<double> &charge);
    [[nodiscard]] Point getGradient(const Point &p) const;

private:
    BBox area;
    std::size_t n;
    double binWidth;
    double binHeight;
    std::vector<double> cosBasis;
    std::vector<double> sinBasis;
    std::vector<double> gradX;
    std::vector<double> gradY;
};

[[nodiscard]] std::size_t getPowerOfTwoAtLeast(std::size_t value);

}  // namespace ktplace::kraftwerk2
