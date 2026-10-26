#pragma once

#include "geometry/kt_bbox.h"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace ktplace::simpl {

struct BinRect {
    std::size_t x0 = 0;
    std::size_t x1 = 0;
    std::size_t y0 = 0;
    std::size_t y1 = 0;

    [[nodiscard]] BinRect getMerged(const BinRect &o) const {
        return {std::min(x0, o.x0), std::max(x1, o.x1), std::min(y0, o.y0), std::max(y1, o.y1)};
    }
    [[nodiscard]] std::size_t getBins() const {
        return (x1 - x0 + 1) * (y1 - y0 + 1);
    }
};

struct AxisProfile {
    std::vector<double> edges;
    std::vector<double> prefix;

    [[nodiscard]] double getTotal() const {
        return prefix.back();
    }
    [[nodiscard]] double getBefore(double c) const {
        if (c <= edges.front()) {
            return 0.0;
        }
        if (c >= edges.back()) {
            return getTotal();
        }
        const std::size_t k = static_cast<std::size_t>(
                                  std::upper_bound(edges.begin(), edges.end(), c) - edges.begin()) -
                              1;
        const double f = (c - edges[k]) / (edges[k + 1] - edges[k]);
        return prefix[k] + f * (prefix[k + 1] - prefix[k]);
    }
    [[nodiscard]] double getBetween(double a, double b) const {
        return getBefore(b) - getBefore(a);
    }
    [[nodiscard]] double getCoordinateAt(double target) const {
        if (target <= 0.0) {
            return edges.front();
        }
        if (target >= getTotal()) {
            return edges.back();
        }
        const std::size_t k = static_cast<std::size_t>(
            std::lower_bound(prefix.begin(), prefix.end(), target) - prefix.begin());
        const double step = prefix[k] - prefix[k - 1];
        const double f = step > 0.0 ? (target - prefix[k - 1]) / step : 1.0;
        return edges[k - 1] + f * (edges[k] - edges[k - 1]);
    }
};

struct BinGrid {
    double x0 = 0.0;
    double y0 = 0.0;
    double dx = 1.0;
    double dy = 1.0;
    std::size_t nx = 1;
    std::size_t ny = 1;
    std::vector<double> avail;
    double totalAvail = 0.0;

    [[nodiscard]] std::size_t size() const {
        return nx * ny;
    }
    [[nodiscard]] std::size_t at(std::size_t ix, std::size_t iy) const {
        return iy * nx + ix;
    }
    [[nodiscard]] std::size_t getColumn(double x) const {
        return clampIndex((x - x0) / dx, nx);
    }
    [[nodiscard]] std::size_t getRow(double y) const {
        return clampIndex((y - y0) / dy, ny);
    }
    [[nodiscard]] std::size_t getBinOf(double x, double y) const {
        return at(getColumn(x), getRow(y));
    }
    [[nodiscard]] double getEdge(bool vertical, std::size_t t) const {
        return vertical ? x0 + static_cast<double>(t) * dx : y0 + static_cast<double>(t) * dy;
    }
    [[nodiscard]] BBox getBox(const BinRect &r) const {
        return {getEdge(true, r.x0), getEdge(false, r.y0), getEdge(true, r.x1 + 1),
                getEdge(false, r.y1 + 1)};
    }
    [[nodiscard]] double sum(const std::vector<double> &field, const BinRect &r) const {
        double total = 0.0;
        for (std::size_t iy = r.y0; iy <= r.y1; ++iy) {
            for (std::size_t ix = r.x0; ix <= r.x1; ++ix) {
                total += field[at(ix, iy)];
            }
        }
        return total;
    }
    void deposit(const BBox &b, double sign) {
        if (!(b.xhi > b.xlo) || !(b.yhi > b.ylo)) {
            return;
        }
        for (std::size_t iy = getRow(b.ylo); iy <= getRow(b.yhi); ++iy) {
            const double oy =
                std::min(b.yhi, getEdge(false, iy + 1)) - std::max(b.ylo, getEdge(false, iy));
            for (std::size_t ix = getColumn(b.xlo); ix <= getColumn(b.xhi); ++ix) {
                const double ox =
                    std::min(b.xhi, getEdge(true, ix + 1)) - std::max(b.xlo, getEdge(true, ix));
                if (ox > 0.0 && oy > 0.0) {
                    avail[at(ix, iy)] += sign * ox * oy;
                }
            }
        }
    }

    static std::size_t clampIndex(double f, std::size_t n) {
        return static_cast<std::size_t>(std::clamp(f, 0.0, static_cast<double>(n) - 1e-9));
    }
};

struct FreeSpace {
    struct Row {
        double y0 = 0.0;
        double y1 = 0.0;
        std::vector<std::pair<double, double>> spans;

        [[nodiscard]] double getLength(double a, double b) const {
            double total = 0.0;
            for (const auto &[s0, s1] : spans) {
                total += std::max(0.0, std::min(b, s1) - std::max(a, s0));
            }
            return total;
        }
    };
    std::vector<Row> rows;

    void build(std::vector<Row> raw, const std::vector<BBox> &obstacles) {
        rows = std::move(raw);
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
            return a.y0 < b.y0;
        });
        for (Row &row : rows) {
            std::vector<std::pair<double, double>> cuts;
            for (const BBox &o : obstacles) {
                if (o.yhi > row.y0 && o.ylo < row.y1) {
                    cuts.emplace_back(o.xlo, o.xhi);
                }
            }
            std::stable_sort(cuts.begin(), cuts.end());
            std::stable_sort(row.spans.begin(), row.spans.end());
            std::vector<std::pair<double, double>> open;
            for (auto [a, b] : row.spans) {
                for (const auto &[c0, c1] : cuts) {
                    if (c1 <= a || c0 >= b) {
                        continue;
                    }
                    if (c0 > a) {
                        open.emplace_back(a, c0);
                    }
                    a = std::max(a, c1);
                }
                if (b > a) {
                    open.emplace_back(a, b);
                }
            }
            row.spans = std::move(open);
        }
    }

    [[nodiscard]] AxisProfile getProfile(const BBox &b, bool vertical) const {
        const auto first =
            std::upper_bound(rows.begin(), rows.end(), b.ylo, [](double v, const Row &r) {
                return v < r.y1;
            });
        const double lo = vertical ? b.xlo : b.ylo;
        const double hi = vertical ? b.xhi : b.yhi;
        AxisProfile out;
        out.edges.push_back(lo);
        out.prefix.push_back(0.0);
        const auto extend = [&out](double to, double area) {
            if (to > out.edges.back()) {
                out.edges.push_back(to);
                out.prefix.push_back(out.prefix.back() + area);
            }
        };
        if (vertical) {
            std::vector<std::pair<double, double>> steps;
            for (auto r = first; r != rows.end() && r->y0 < b.yhi; ++r) {
                const double h = std::min(b.yhi, r->y1) - std::max(b.ylo, r->y0);
                for (const auto &[s0, s1] : r->spans) {
                    const double a = std::max(s0, lo);
                    const double z = std::min(s1, hi);
                    if (h > 0.0 && z > a) {
                        steps.emplace_back(a, h);
                        steps.emplace_back(z, -h);
                    }
                }
            }
            std::stable_sort(steps.begin(), steps.end());
            double height = 0.0;
            for (const auto &[at, dh] : steps) {
                extend(at, height * (at - out.edges.back()));
                height += dh;
            }
        } else {
            for (auto r = first; r != rows.end() && r->y0 < b.yhi; ++r) {
                const double a = std::max(lo, r->y0);
                const double z = std::min(hi, r->y1);
                if (z > a) {
                    extend(a, 0.0);
                    extend(z, r->getLength(b.xlo, b.xhi) * (z - a));
                }
            }
        }
        extend(hi, 0.0);
        if (out.edges.size() == 1) {
            out.edges.push_back(hi);
            out.prefix.push_back(0.0);
        }
        return out;
    }
};

}  // namespace ktplace::simpl
