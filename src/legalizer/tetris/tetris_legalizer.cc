#include "legalizer/tetris/tetris_legalizer.h"

#include "datamodel/kt_hpwl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

namespace ktplace::tetris {

namespace {

constexpr double eps = 1e-9;
constexpr std::size_t noRow = std::numeric_limits<std::size_t>::max();

struct Segment {
    Interval x;
    int region = noRegion;
    std::vector<Interval> gaps;
};

struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double sitePitch = 1.0;
    std::vector<Segment> segments;
};

struct Candidate {
    double cost = std::numeric_limits<double>::infinity();
    std::size_t row = noRow;
    std::size_t segment = 0;
    std::size_t gap = 0;
    double x = 0.0;
};

struct Stats {
    std::size_t cells = 0;
    std::size_t unplaced = 0;
    std::size_t nearestFallback = 0;
    double totalDisplacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

double snapUp(double x, double pitch) {
    return std::ceil(x / pitch - eps) * pitch;
}

void report(const Stats &s) {
    ktReportTable t("Legalization (Tetris)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells placed", std::format("{}", s.cells - s.unplaced)});
    t.addRow({"cells unplaced", std::format("{}", s.unplaced)});
    t.addRow({"placed at the nearest free site", std::format("{}", s.nearestFallback)});
    t.addRow({"total displacement", std::format("{:.6e}", s.totalDisplacement)});
    t.addRow({"max displacement", std::format("{:.1f}", s.maxDisplacement)});
    t.addRow({"HPWL before", std::format("{:.6e}", s.hpwlBefore)});
    t.addRow({"HPWL after", std::format("{:.6e}", s.hpwlAfter)});
    t.addRow(
        {"HPWL change", std::format("{:+.2f}%", 100.0 * (s.hpwlAfter - s.hpwlBefore) /
                                                    (s.hpwlBefore > 0.0 ? s.hpwlBefore : 1.0))});
    t.addRow({"seconds", std::format("{:.3f}", s.seconds)});
    t.emit();

    if (s.unplaced > 0) {
        ktlog.warning(
            "{} cell(s) fit in no row and are still at their global placement positions; the "
            "placement is not legal",
            s.unplaced);
    }
}

}  // namespace

class TetrisLegalizer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    Stats run(const TetrisOptions &options);

private:
    [[nodiscard]] std::vector<RowTrack> buildRows() const;
    [[nodiscard]] std::vector<std::size_t> getOrder(double widthFactor) const;
    [[nodiscard]] std::size_t getHomeRow(double y) const;
    [[nodiscard]] Candidate findSite(std::size_t cell, double boundary, double yWeight,
                                     bool nearest) const;
    void tryRow(std::size_t row, std::size_t cell, double boundary, double yWeight, bool nearest,
                Candidate &best) const;
    void tryNearest(std::size_t row, std::size_t cell, double rowCost, Candidate &best) const;
    void occupy(const Candidate &site, double width);
    [[nodiscard]] double getHpwlOf(const std::vector<double> &xs,
                                   const std::vector<double> &ys) const;
    void writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                    const std::string &note, std::size_t step) const;

    Design &design;
    std::vector<RowTrack> rows;
};

std::vector<RowTrack> TetrisLegalizer::Impl::buildRows() const {
    std::vector<RowTrack> out;
    for (const RowInfo &info : design.rows) {
        RowTrack track;
        track.y = info.coordinate;
        track.height = info.height;
        track.sitePitch = info.sitePitch > 0.0 ? info.sitePitch : 1.0;
        const auto addSegment = [&track](double start, double end, int region) {
            if (end - start > eps) {
                Segment segment;
                segment.x = Interval(start, end);
                segment.region = region;
                segment.gaps.push_back(segment.x);
                track.segments.push_back(std::move(segment));
            }
        };
        for (const Subrow &subrow : info.subrows) {
            std::vector<Interval> blocked;
            for (const BBox &box : design.fixedBoxes) {
                if (box.upperRight.y <= track.y + eps ||
                    box.lowerLeft.y >= track.y + track.height - eps) {
                    continue;
                }
                const double start = std::max(box.lowerLeft.x, subrow.x.start);
                const double end = std::min(box.upperRight.x, subrow.x.end);
                if (end > start) {
                    blocked.emplace_back(start, end);
                }
            }
            std::stable_sort(blocked.begin(), blocked.end());
            double from = subrow.x.start;
            for (const Interval &block : blocked) {
                addSegment(from, block.start, subrow.region);
                from = std::max(from, block.end);
            }
            addSegment(from, subrow.x.end, subrow.region);
        }
        if (!track.segments.empty()) {
            std::ranges::stable_sort(track.segments, {}, [](const Segment &segment) {
                return segment.x.start;
            });
            out.push_back(std::move(track));
        }
    }
    std::ranges::stable_sort(out, {}, &RowTrack::y);
    return out;
}

std::vector<std::size_t> TetrisLegalizer::Impl::getOrder(double widthFactor) const {
    const Cells &cells = design.cells;
    std::vector<double> key(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) {
        key[i] = cells.x[i] - widthFactor * cells.width[i];
    }
    std::vector<std::size_t> order(cells.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&key](std::size_t a, std::size_t b) {
        return key[a] < key[b];
    });
    return order;
}

std::size_t TetrisLegalizer::Impl::getHomeRow(double y) const {
    const auto above = std::ranges::lower_bound(rows, y, {}, &RowTrack::y);
    const auto k = static_cast<std::size_t>(above - rows.begin());
    if (k == rows.size()) {
        return k - 1;
    }
    if (k > 0 && y - rows[k - 1].y <= rows[k].y - y) {
        return k - 1;
    }
    return k;
}

void TetrisLegalizer::Impl::tryRow(std::size_t row, std::size_t cell, double boundary,
                                   double yWeight, bool nearest, Candidate &best) const {
    const RowTrack &track = rows[row];
    const Cells &cells = design.cells;
    if (cells.height[cell] > track.height + eps) {
        return;
    }
    const double dy = track.y - cells.y[cell];
    const double rowCost = yWeight * dy * dy;
    if (rowCost >= best.cost) {
        return;
    }
    if (nearest) {
        tryNearest(row, cell, rowCost, best);
        return;
    }
    for (std::size_t s = 0; s < track.segments.size(); ++s) {
        const Segment &segment = track.segments[s];
        if (segment.region != cells.region[cell]) {
            continue;
        }
        auto gap = std::ranges::upper_bound(segment.gaps, boundary, {}, &Interval::end);
        for (; gap != segment.gaps.end(); ++gap) {
            const double x = snapUp(std::max(gap->start, boundary), track.sitePitch);
            if (x + cells.width[cell] > gap->end + eps) {
                continue;
            }
            const double dx = x - cells.x[cell];
            const double cost = dx * dx + rowCost;
            if (cost < best.cost) {
                best = Candidate{.cost = cost,
                                 .row = row,
                                 .segment = s,
                                 .gap = static_cast<std::size_t>(gap - segment.gaps.begin()),
                                 .x = x};
            }
            return;
        }
    }
}

void TetrisLegalizer::Impl::tryNearest(std::size_t row, std::size_t cell, double rowCost,
                                       Candidate &best) const {
    const RowTrack &track = rows[row];
    const Cells &cells = design.cells;
    const double pitch = track.sitePitch;
    for (std::size_t s = 0; s < track.segments.size(); ++s) {
        const Segment &segment = track.segments[s];
        if (segment.region != cells.region[cell]) {
            continue;
        }
        for (std::size_t g = 0; g < segment.gaps.size(); ++g) {
            const Interval &gap = segment.gaps[g];
            const double lo = snapUp(gap.start, pitch);
            const double hi = std::floor((gap.end - cells.width[cell]) / pitch + eps) * pitch;
            if (hi < lo - eps) {
                continue;
            }
            const double x = std::clamp(std::round(cells.x[cell] / pitch) * pitch, lo, hi);
            const double dx = x - cells.x[cell];
            const double cost = dx * dx + rowCost;
            if (cost < best.cost) {
                best = Candidate{.cost = cost, .row = row, .segment = s, .gap = g, .x = x};
            }
        }
    }
}

Candidate TetrisLegalizer::Impl::findSite(std::size_t cell, double boundary, double yWeight,
                                          bool nearest) const {
    Candidate best;
    const double y = design.cells.y[cell];
    const std::size_t home = getHomeRow(y);
    tryRow(home, cell, boundary, yWeight, nearest, best);
    for (std::size_t step = 1; step < rows.size(); ++step) {
        const bool up = home + step < rows.size();
        const bool down = step <= home;
        if (!up && !down) {
            break;
        }
        const double dyUp = up ? rows[home + step].y - y : std::numeric_limits<double>::max();
        const double dyDown = down ? y - rows[home - step].y : std::numeric_limits<double>::max();
        const double closest = std::min(std::abs(dyUp), std::abs(dyDown));
        if (yWeight * closest * closest >= best.cost) {
            break;
        }
        if (up) {
            tryRow(home + step, cell, boundary, yWeight, nearest, best);
        }
        if (down) {
            tryRow(home - step, cell, boundary, yWeight, nearest, best);
        }
    }
    return best;
}

void TetrisLegalizer::Impl::occupy(const Candidate &site, double width) {
    std::vector<Interval> &gaps = rows[site.row].segments[site.segment].gaps;
    const Interval gap = gaps[site.gap];
    std::vector<Interval> rest;
    if (site.x - gap.start > eps) {
        rest.emplace_back(gap.start, site.x);
    }
    if (gap.end - (site.x + width) > eps) {
        rest.emplace_back(site.x + width, gap.end);
    }
    const auto at = gaps.erase(gaps.begin() + static_cast<std::ptrdiff_t>(site.gap));
    gaps.insert(at, rest.begin(), rest.end());
}

double TetrisLegalizer::Impl::getHpwlOf(const std::vector<double> &xs,
                                        const std::vector<double> &ys) const {
    return getNetHpwl(
        design.nets.size(),
        [this](std::size_t n) {
            return design.nets[n];
        },
        [&](const NetPin &pin) {
            return pin.slot == noSlot
                       ? pin.fixedPosition
                       : Point(xs[pin.slot] + pin.offset.x, ys[pin.slot] + pin.offset.y);
        });
}

void TetrisLegalizer::Impl::writeFrame(const std::vector<double> &xs, const std::vector<double> &ys,
                                       const std::string &note, std::size_t step) const {
    const Cells &cells = design.cells;
    std::vector<float> fx(design.vertexX.size());
    std::vector<float> fy(design.vertexY.size());
    for (std::size_t v = 0; v < fx.size(); ++v) {
        fx[v] = static_cast<float>(design.vertexX[v]);
        fy[v] = static_cast<float>(design.vertexY[v]);
    }
    for (std::size_t i = 0; i < cells.size(); ++i) {
        fx[cells.id[i]] = static_cast<float>(xs[i]);
        fy[cells.id[i]] = static_cast<float>(ys[i]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.numSteps = cells.size();
    info.hpwl = getHpwlOf(xs, ys);
    info.hpwlInitial = getHpwlOf(cells.x, cells.y);
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

Stats TetrisLegalizer::Impl::run(const TetrisOptions &options) {
    ScopedTimer timer("tetris");
    Cells &cells = design.cells;
    Stats stats;
    stats.cells = cells.size();
    std::vector<double> xs = cells.x;
    std::vector<double> ys = cells.y;
    stats.hpwlBefore = getHpwlOf(xs, ys);
    rows = buildRows();
    if (design.onFrame) {
        writeFrame(xs, ys, "Input - global placement", 0);
    }

    const double widest = cells.size() == 0 ? 0.0 : std::ranges::max(cells.width);
    const std::size_t frameInterval =
        options.frames > 0 ? (cells.size() + options.frames - 1) / options.frames : 0;
    std::size_t visited = 0;
    for (const std::size_t i : getOrder(options.widthFactor)) {
        ++visited;
        Candidate site;
        if (!rows.empty()) {
            site = findSite(i, cells.x[i] - options.leftFactor * widest, options.yWeight, false);
            if (site.row == noRow) {
                site = findSite(i, 0.0, options.yWeight, true);
                stats.nearestFallback += site.row != noRow ? 1 : 0;
            }
        }
        if (site.row == noRow) {
            ++stats.unplaced;
        } else {
            occupy(site, cells.width[i]);
            xs[i] = site.x;
            ys[i] = rows[site.row].y;
        }
        if (design.onFrame && frameInterval > 0 && visited % frameInterval == 0) {
            writeFrame(xs, ys, "Placing cells into rows", visited);
        }
    }

    for (std::size_t i = 0; i < cells.size(); ++i) {
        const double dx = xs[i] - cells.x[i];
        const double dy = ys[i] - cells.y[i];
        const double moved = std::sqrt(dx * dx + dy * dy);
        stats.totalDisplacement += moved;
        stats.maxDisplacement = std::max(stats.maxDisplacement, moved);
    }
    stats.hpwlAfter = getHpwlOf(xs, ys);
    if (design.onFrame) {
        writeFrame(xs, ys, "Legal placement", cells.size() - stats.unplaced);
    }
    cells.x = std::move(xs);
    cells.y = std::move(ys);
    stats.seconds = timer.getElapsedSeconds();
    return stats;
}

TetrisLegalizer::TetrisLegalizer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

TetrisLegalizer::~TetrisLegalizer() = default;

void TetrisLegalizer::legalize(const TetrisOptions &options) {
    report(pImpl->run(options));
}

}  // namespace ktplace::tetris
