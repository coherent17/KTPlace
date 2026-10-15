// @file kt_rowFence.h
// Cutting a row into pieces at fence boundaries.
//
// Fences are exclusive, as in the ISPD 2015 contest: a region's member cells
// must lie inside it, and every other cell outside. A row-based stage (the
// legalizer, the detail placer) enforces that by giving each stretch of a row a
// label -- the region it belongs to, or open space -- and only letting a cell
// use stretches with its own label. This is that labelling. ktDM::fencedSubrows()
// applies it once, and every row-based stage receives the labelled pieces through
// its DM builder, so the legalizers and the detailed placer always agree on what
// is legal next to a fence.

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace ktplace {

/// One rectangle of a fence region, with the region's id.
struct FenceRect {
    double xlo = 0.0, ylo = 0.0, xhi = 0.0, yhi = 0.0;
    int region = -1;
};

/// A labelled stretch of a row: [xlo, xhi) belonging to `region`, or to no
/// region (open space) when region is kOpenSpace.
struct RowPiece {
    double xlo = 0.0, xhi = 0.0;
    int region = -1;
};

inline constexpr int kOpenSpace = -1;

/// Split [xlo, xhi) of the row band [y0, y1) by the fence rectangles.
///
/// A rectangle covering the whole band makes its x-interval member space for its
/// region. A rectangle reaching into the band only partly makes it unusable for
/// everyone: a member there would stick out of its region, and any other cell
/// would sit inside it. Edges made by a fence are snapped inward to the site
/// grid, so a cell placed in a piece lies wholly on its side of the boundary;
/// the piece's own ends (xlo, xhi) are kept as given. Unusable stretches and
/// slivers narrower than a site are dropped.
inline std::vector<RowPiece> splitRowByFences(double xlo, double xhi, double y0, double y1,
                                              double grid, const std::vector<FenceRect> &rects) {
    constexpr double eps = 1e-9;
    constexpr int kBlocked = -2;
    struct Span {
        double lo, hi;
        int region;  // member region, or kBlocked
    };
    std::vector<Span> spans;
    for (const FenceRect &rc : rects) {
        if (!(rc.yhi > y0 + eps && rc.ylo < y1 - eps) || !(rc.xhi > xlo && rc.xlo < xhi)) {
            continue;
        }
        const bool covers = rc.ylo <= y0 + eps && rc.yhi >= y1 - eps;
        spans.push_back({rc.xlo, rc.xhi, covers ? rc.region : kBlocked});
    }
    if (spans.empty()) {
        return {RowPiece{xlo, xhi, kOpenSpace}};
    }
    std::vector<double> cuts{xlo, xhi};
    for (const Span &sp : spans) {
        if (sp.lo > xlo && sp.lo < xhi) {
            cuts.push_back(sp.lo);
        }
        if (sp.hi > xlo && sp.hi < xhi) {
            cuts.push_back(sp.hi);
        }
    }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    // Label by midpoint: member space wins over a blocking rectangle, which wins
    // over open space.
    const auto labelOf = [&](double mid) {
        int label = kOpenSpace;
        for (const Span &sp : spans) {
            if (mid <= sp.lo || mid >= sp.hi) {
                continue;
            }
            if (sp.region != kBlocked) {
                return sp.region;
            }
            label = kBlocked;
        }
        return label;
    };
    const auto snapLo = [&](double v) {
        return (v == xlo || !(grid > 0.0)) ? v : std::ceil(v / grid - 1e-9) * grid;
    };
    const auto snapHi = [&](double v) {
        return (v == xhi || !(grid > 0.0)) ? v : std::floor(v / grid + 1e-9) * grid;
    };
    std::vector<RowPiece> out;
    std::size_t k = 0;
    while (k + 1 < cuts.size()) {
        const int label = labelOf(0.5 * (cuts[k] + cuts[k + 1]));
        std::size_t e = k + 1;
        while (e + 1 < cuts.size() && labelOf(0.5 * (cuts[e] + cuts[e + 1])) == label) {
            ++e;
        }
        if (label != kBlocked) {
            const double lo = snapLo(cuts[k]), hi = snapHi(cuts[e]);
            if (hi > lo + eps) {
                out.push_back(RowPiece{lo, hi, label});
            }
        }
        k = e;
    }
    return out;
}

}  // namespace ktplace
