// @file kt_layoutSnapshot.cc
// Implementation of LayoutSnapshot: the copy and the picking grid.

#include "gui/kt_layoutSnapshot.h"

#include "session/kt_session.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace ktplace {

namespace {

float median(std::vector<float> values, float fallback) {
    if (values.empty()) {
        return fallback;
    }
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid > 0.0F ? *mid : fallback;
}

}  // namespace

std::shared_ptr<const LayoutSnapshot> LayoutSnapshot::capture(const ktSession &session) {
    auto snap = std::make_shared<LayoutSnapshot>();
    snap->die = session.placementDieBox();
    snap->hpwl = session.hpwl();

    const auto cells = session.cells();
    snap->cells.reserve(cells.size());
    std::vector<float> widths;
    std::vector<float> heights;
    for (const Vertex &v : cells) {
        Cell c;
        c.x = static_cast<float>(v.x);
        c.y = static_cast<float>(v.y);
        c.w = static_cast<float>(v.width);
        c.h = static_cast<float>(v.height);
        c.kind = v.isTerminal ? Kind::Terminal : (v.isFixed ? Kind::Fixed : Kind::Movable);
        if (c.kind == Kind::Movable) {
            widths.push_back(c.w);
            heights.push_back(c.h);
        }
        snap->cells.push_back(c);
    }
    snap->typicalWidth = median(std::move(widths), 1.0F);
    snap->typicalHeight = median(std::move(heights), 1.0F);

    // Bookshelf marks every fixed object "terminal", macros included (all of
    // ISPD 2005's are), so the flag alone would draw a macro as a pad. A
    // terminal bigger than a few standard cells is drawn as the macro it is.
    const float macroArea = 16.0F * snap->typicalWidth * snap->typicalHeight;
    for (Cell &c : snap->cells) {
        if (c.kind == Kind::Terminal && c.w * c.h > macroArea) {
            c.kind = Kind::Fixed;
        }
    }

    for (const RowInfo &r : session.die().getRows()) {
        snap->rows.push_back({static_cast<float>(r.coordinate), static_cast<float>(r.height),
                              static_cast<float>(r.xlo()), static_cast<float>(r.xhi())});
    }
    snap->buildIndex();
    return snap;
}

int LayoutSnapshot::binOf(double v, double lo, double size, int count) const {
    const int b = static_cast<int>(std::floor((v - lo) / size));
    return std::clamp(b, 0, count - 1);
}

void LayoutSnapshot::buildIndex() {
    const double w = std::max(die[2] - die[0], 1e-9);
    const double h = std::max(die[3] - die[1], 1e-9);
    // About eight cells per bin, in a grid shaped like the die.
    const double bins = std::max(1.0, static_cast<double>(cells.size()) / 8.0);
    const double aspect = w / h;
    gridX_ = std::clamp(static_cast<int>(std::sqrt(bins * aspect)), 1, 4096);
    gridY_ = std::clamp(static_cast<int>(std::sqrt(bins / aspect)), 1, 4096);
    binW_ = w / gridX_;
    binH_ = h / gridY_;

    const std::size_t nbins = static_cast<std::size_t>(gridX_) * static_cast<std::size_t>(gridY_);
    std::vector<std::uint32_t> counts(nbins + 1, 0);
    // Two passes, count then fill, so the index is two flat arrays.
    const auto forEachBin = [&](const Cell &c, auto &&fn) {
        const int x0 = binOf(c.x, die[0], binW_, gridX_);
        const int x1 = binOf(c.x + c.w, die[0], binW_, gridX_);
        const int y0 = binOf(c.y, die[1], binH_, gridY_);
        const int y1 = binOf(c.y + c.h, die[1], binH_, gridY_);
        for (int by = y0; by <= y1; ++by) {
            for (int bx = x0; bx <= x1; ++bx) {
                fn(static_cast<std::size_t>(by) * static_cast<std::size_t>(gridX_) +
                   static_cast<std::size_t>(bx));
            }
        }
    };
    for (const Cell &c : cells) {
        forEachBin(c, [&](std::size_t b) {
            ++counts[b + 1];
        });
    }
    for (std::size_t b = 0; b < nbins; ++b) {
        counts[b + 1] += counts[b];
    }
    binStart_ = counts;
    binCells_.assign(binStart_.back(), 0);
    std::vector<std::uint32_t> fill(binStart_.begin(), binStart_.end() - 1);
    for (std::size_t i = 0; i < cells.size(); ++i) {
        forEachBin(cells[i], [&](std::size_t b) {
            binCells_[fill[b]++] = static_cast<std::uint32_t>(i);
        });
    }
}

std::optional<std::size_t> LayoutSnapshot::cellAt(double x, double y) const {
    if (binStart_.empty()) {
        return std::nullopt;
    }
    const std::size_t b = static_cast<std::size_t>(binOf(y, die[1], binH_, gridY_)) *
                              static_cast<std::size_t>(gridX_) +
                          static_cast<std::size_t>(binOf(x, die[0], binW_, gridX_));
    std::optional<std::size_t> best;
    double bestArea = std::numeric_limits<double>::max();
    for (std::uint32_t k = binStart_[b]; k < binStart_[b + 1]; ++k) {
        const Cell &c = cells[binCells_[k]];
        if (x >= c.x && x <= c.x + c.w && y >= c.y && y <= c.y + c.h) {
            const double area = static_cast<double>(c.w) * c.h;
            if (area < bestArea) {
                bestArea = area;
                best = binCells_[k];
            }
        }
    }
    return best;
}

}  // namespace ktplace
