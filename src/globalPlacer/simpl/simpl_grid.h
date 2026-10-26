#pragma once

#include "geometry/kt_geometry.h"

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
        return {.x0 = std::min(x0, o.x0),
                .x1 = std::max(x1, o.x1),
                .y0 = std::min(y0, o.y0),
                .y1 = std::max(y1, o.y1)};
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
    Point origin;
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
        return clampIndex((x - origin.x) / dx, nx);
    }
    [[nodiscard]] std::size_t getRow(double y) const {
        return clampIndex((y - origin.y) / dy, ny);
    }
    [[nodiscard]] std::size_t getBinOf(const Point &p) const {
        return at(getColumn(p.x), getRow(p.y));
    }
    [[nodiscard]] double getEdge(bool vertical, std::size_t t) const {
        return vertical ? origin.x + static_cast<double>(t) * dx
                        : origin.y + static_cast<double>(t) * dy;
    }
    [[nodiscard]] BBox getBox(const BinRect &r) const {
        return BBox(Point(getEdge(true, r.x0), getEdge(false, r.y0)),
                    Point(getEdge(true, r.x1 + 1), getEdge(false, r.y1 + 1)));
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
        if (!(b.upperRight.x > b.lowerLeft.x) || !(b.upperRight.y > b.lowerLeft.y)) {
            return;
        }
        for (std::size_t iy = getRow(b.lowerLeft.y); iy <= getRow(b.upperRight.y); ++iy) {
            const double oy = std::min(b.upperRight.y, getEdge(false, iy + 1)) -
                              std::max(b.lowerLeft.y, getEdge(false, iy));
            for (std::size_t ix = getColumn(b.lowerLeft.x); ix <= getColumn(b.upperRight.x); ++ix) {
                const double ox = std::min(b.upperRight.x, getEdge(true, ix + 1)) -
                                  std::max(b.lowerLeft.x, getEdge(true, ix));
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
        Interval y;
        std::vector<Interval> spans;

        [[nodiscard]] double getLength(const Interval &x) const {
            double total = 0.0;
            for (const Interval &span : spans) {
                total += std::max(0.0, std::min(x.end, span.end) - std::max(x.start, span.start));
            }
            return total;
        }
    };
    std::vector<Row> rows;

    void build(std::vector<Row> raw, const std::vector<BBox> &obstacles) {
        rows = std::move(raw);
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
            return a.y.start < b.y.start;
        });
        for (Row &row : rows) {
            std::vector<Interval> cuts;
            for (const BBox &o : obstacles) {
                if (o.upperRight.y > row.y.start && o.lowerLeft.y < row.y.end) {
                    cuts.push_back(o.getX());
                }
            }
            std::stable_sort(cuts.begin(), cuts.end());
            std::stable_sort(row.spans.begin(), row.spans.end());
            std::vector<Interval> open;
            for (const Interval &span : row.spans) {
                double a = span.start;
                for (const Interval &cut : cuts) {
                    if (cut.end <= a || cut.start >= span.end) {
                        continue;
                    }
                    if (cut.start > a) {
                        open.emplace_back(a, cut.start);
                    }
                    a = std::max(a, cut.end);
                }
                if (span.end > a) {
                    open.emplace_back(a, span.end);
                }
            }
            row.spans = std::move(open);
        }
    }

    [[nodiscard]] AxisProfile getProfile(const BBox &b, bool vertical) const {
        const auto first =
            std::upper_bound(rows.begin(), rows.end(), b.lowerLeft.y, [](double v, const Row &r) {
                return v < r.y.end;
            });
        const double lo = vertical ? b.lowerLeft.x : b.lowerLeft.y;
        const double hi = vertical ? b.upperRight.x : b.upperRight.y;
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
            for (auto r = first; r != rows.end() && r->y.start < b.upperRight.y; ++r) {
                const double h =
                    std::min(b.upperRight.y, r->y.end) - std::max(b.lowerLeft.y, r->y.start);
                for (const Interval &span : r->spans) {
                    const double a = std::max(span.start, lo);
                    const double z = std::min(span.end, hi);
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
            for (auto r = first; r != rows.end() && r->y.start < b.upperRight.y; ++r) {
                const double a = std::max(lo, r->y.start);
                const double z = std::min(hi, r->y.end);
                if (z > a) {
                    extend(a, 0.0);
                    extend(z, r->getLength(b.getX()) * (z - a));
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
