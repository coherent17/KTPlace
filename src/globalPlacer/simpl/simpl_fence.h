#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ktplace::simpl {

struct Rect {
    double xlo = 0.0;
    double ylo = 0.0;
    double xhi = 0.0;
    double yhi = 0.0;

    [[nodiscard]] bool contains(double x, double y) const noexcept {
        return x >= xlo && x <= xhi && y >= ylo && y <= yhi;
    }
    [[nodiscard]] double width() const noexcept {
        return xhi - xlo;
    }
    [[nodiscard]] double height() const noexcept {
        return yhi - ylo;
    }
};

struct Region {
    std::string name;
    std::vector<Rect> rects;
    double minX = 0.0;
    double minY = 0.0;
    double maxX = 0.0;
    double maxY = 0.0;

    [[nodiscard]] bool contains(double x, double y) const {
        if (x < minX || x > maxX || y < minY || y > maxY) {
            return false;
        }
        for (const Rect &r : rects) {
            if (r.contains(x, y)) {
                return true;
            }
        }
        return false;
    }
    void seal();
};

class Fences {
public:
    static constexpr int noRegion = -1;

    [[nodiscard]] std::size_t numRegions() const noexcept {
        return regions.size();
    }
    [[nodiscard]] const Region &region(int id) const {
        return regions.at(static_cast<std::size_t>(id));
    }

    bool clampToRegion(int id, double &x, double &y) const;

    bool pushOutOfRegions(double &x, double &y, double dieMinX, double dieMinY, double maxX,
                          double maxY) const;

    [[nodiscard]] std::size_t countViolations(const std::vector<double> &positions,
                                              const std::vector<int> &regionIds) const;

    void addRegion(Region r) {
        regions.push_back(std::move(r));
    }

private:
    std::vector<Region> regions;
};

}  // namespace ktplace::simpl
