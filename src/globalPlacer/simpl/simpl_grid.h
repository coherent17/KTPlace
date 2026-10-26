#pragma once

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace ktplace::simpl {

struct Box {
    double x0 = 0.0;
    double y0 = 0.0;
    double x1 = 0.0;
    double y1 = 0.0;
};

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
    [[nodiscard]] Box getBox(const BinRect &r) const {
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
    void deposit(const Box &b, double sign) {
        if (!(b.x1 > b.x0) || !(b.y1 > b.y0)) {
            return;
        }
        for (std::size_t iy = getRow(b.y0); iy <= getRow(b.y1); ++iy) {
            const double oy =
                std::min(b.y1, getEdge(false, iy + 1)) - std::max(b.y0, getEdge(false, iy));
            for (std::size_t ix = getColumn(b.x0); ix <= getColumn(b.x1); ++ix) {
                const double ox =
                    std::min(b.x1, getEdge(true, ix + 1)) - std::max(b.x0, getEdge(true, ix));
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

    void build(std::vector<Row> raw, const std::vector<Box> &obstacles) {
        rows = std::move(raw);
        std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
            return a.y0 < b.y0;
        });
        for (Row &row : rows) {
            std::vector<std::pair<double, double>> cuts;
            for (const Box &o : obstacles) {
                if (o.y1 > row.y0 && o.y0 < row.y1) {
                    cuts.emplace_back(o.x0, o.x1);
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

    [[nodiscard]] AxisProfile getProfile(const Box &b, bool vertical) const {
        const auto first =
            std::upper_bound(rows.begin(), rows.end(), b.y0, [](double v, const Row &r) {
                return v < r.y1;
            });
        const double lo = vertical ? b.x0 : b.y0;
        const double hi = vertical ? b.x1 : b.y1;
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
            for (auto r = first; r != rows.end() && r->y0 < b.y1; ++r) {
                const double h = std::min(b.y1, r->y1) - std::max(b.y0, r->y0);
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
            for (auto r = first; r != rows.end() && r->y0 < b.y1; ++r) {
                const double a = std::max(lo, r->y0);
                const double z = std::min(hi, r->y1);
                if (z > a) {
                    extend(a, 0.0);
                    extend(z, r->getLength(b.x0, b.x1) * (z - a));
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
