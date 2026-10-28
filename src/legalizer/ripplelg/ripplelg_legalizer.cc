#include "legalizer/ripplelg/ripplelg_legalizer.h"

#include "datamodel/kt_hpwl.h"
#include "legalizer/ripplelg/ripplelg_flow.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <string>
#include <tuple>
#include <vector>

namespace ktplace::ripplelg {

namespace {

constexpr double eps = 1e-6;
constexpr double inf = std::numeric_limits<double>::infinity();
constexpr std::uint32_t none = std::numeric_limits<std::uint32_t>::max();

struct Piece {
    Interval x;
    int region = noRegion;
};

struct StageResult {
    std::string name;
    double displacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwl = 0.0;
    double seconds = 0.0;
};

struct Stats {
    std::size_t movable = 0;
    std::size_t multiRow = 0;
    std::size_t tall = 0;
    std::size_t unplaced = 0;
    std::size_t windowGrowths = 0;
    std::size_t matched = 0;
    double hpwlBefore = 0.0;
    std::vector<StageResult> stages;
};

double getPhi(double delta, double tolerated) {
    if (delta <= tolerated) {
        return delta;
    }
    const double ratio = delta / tolerated;
    return delta * ratio * ratio * ratio * ratio;
}

std::vector<std::size_t> solveAssignment(const std::vector<double> &cost, std::size_t n) {
    std::vector<double> u(n + 1, 0.0);
    std::vector<double> v(n + 1, 0.0);
    std::vector<std::size_t> column(n + 1, 0);
    std::vector<std::size_t> way(n + 1, 0);
    for (std::size_t row = 1; row <= n; ++row) {
        column[0] = row;
        std::size_t j0 = 0;
        std::vector<double> least(n + 1, inf);
        std::vector<char> used(n + 1, 0);
        do {
            used[j0] = 1;
            const std::size_t i0 = column[j0];
            double delta = inf;
            std::size_t j1 = 0;
            for (std::size_t j = 1; j <= n; ++j) {
                if (used[j] != 0) {
                    continue;
                }
                const double reduced = cost[(i0 - 1) * n + (j - 1)] - u[i0] - v[j];
                if (reduced < least[j]) {
                    least[j] = reduced;
                    way[j] = j0;
                }
                if (least[j] < delta) {
                    delta = least[j];
                    j1 = j;
                }
            }
            for (std::size_t j = 0; j <= n; ++j) {
                if (used[j] != 0) {
                    u[column[j]] += delta;
                    v[j] -= delta;
                } else {
                    least[j] -= delta;
                }
            }
            j0 = j1;
        } while (column[j0] != 0);
        do {
            const std::size_t j1 = way[j0];
            column[j0] = column[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    std::vector<std::size_t> assigned(n, 0);
    for (std::size_t j = 1; j <= n; ++j) {
        assigned[column[j] - 1] = j - 1;
    }
    return assigned;
}

void report(const Stats &s) {
    ktReportTable t("Legalization (RippleLG)");
    t.setHeaders({"stage", "total displacement", "max displacement", "HPWL", "seconds"});
    t.addRow({"global placement", "", "", std::format("{:.6e}", s.hpwlBefore), ""});
    for (const StageResult &stage : s.stages) {
        t.addRow({stage.name, std::format("{:.6e}", stage.displacement),
                  std::format("{:.1f}", stage.maxDisplacement), std::format("{:.6e}", stage.hpwl),
                  std::format("{:.3f}", stage.seconds)});
    }
    t.emit();
    ktReportTable counts("RippleLG cells");
    counts.setHeaders({"metric", "value"});
    counts.addRow({"movable cells", std::format("{}", s.movable)});
    counts.addRow({"multi-row cells", std::format("{}", s.multiRow)});
    counts.addRow({"taller than max_rows, placed first", std::format("{}", s.tall)});
    counts.addRow({"window enlargements", std::format("{}", s.windowGrowths)});
    counts.addRow({"cells moved by matching", std::format("{}", s.matched)});
    counts.addRow({"cells unplaced", std::format("{}", s.unplaced)});
    counts.emit();
    if (s.unplaced > 0) {
        ktlog.warning(
            "RippleLG: {} cell(s) found no insertion point and kept their global placement",
            s.unplaced);
    }
}

}  // namespace

class RippleLegalizer::Impl {
public:
    explicit Impl(Design &d) : design(d) {}

    Stats run(const RipplelgOptions &options);

private:
    struct Insertion {
        double cost = inf;
        std::size_t bottomRow = 0;
        double x = 0.0;
        std::vector<std::pair<std::uint32_t, double>> moved;
    };

    [[nodiscard]] std::size_t getSpan(double height) const;
    [[nodiscard]] std::size_t getNearestRow(double y) const;
    [[nodiscard]] bool isStacked(std::size_t bottomRow, std::size_t rowsSpanned) const;
    [[nodiscard]] double snapUp(double x, std::size_t row) const;
    [[nodiscard]] double snapDown(double x, std::size_t row) const;
    void buildPieces();
    void placeTallCells(std::vector<std::uint32_t> tall);
    [[nodiscard]] Insertion findInsertion(std::uint32_t t, double halfWidth,
                                          std::size_t halfRows) const;
    void commit(std::uint32_t t, const Insertion &best);
    void legalizeCells(std::vector<std::uint32_t> cells, Stats &stats);
    std::size_t matchDisplacement();
    bool refineFixedOrder();
    [[nodiscard]] std::size_t getSlot(std::size_t row, std::uint32_t cell) const;
    void recordStage(const std::string &name, double seconds, Stats &stats) const;
    [[nodiscard]] double getHpwl() const;
    void writeFrame(const std::string &note, std::size_t step) const;

    Design &design;
    RipplelgOptions opts;
    double rowHeight = 1.0;
    std::vector<double> xs;
    std::vector<double> ys;
    std::vector<double> gx;
    std::vector<double> gy;
    std::vector<std::size_t> spanOf;
    std::vector<std::size_t> bottomOf;
    std::vector<char> small;
    std::vector<char> placed;
    std::vector<BBox> blocked;
    std::vector<std::vector<Piece>> pieces;
    std::vector<double> rowOrigin;
    std::vector<double> rowPitch;
    std::vector<std::vector<std::uint32_t>> rowCells;
    double widest = 0.0;
};

std::size_t RippleLegalizer::Impl::getSpan(double height) const {
    return static_cast<std::size_t>(std::max(1.0, std::ceil(height / rowHeight - 1e-6)));
}

std::size_t RippleLegalizer::Impl::getNearestRow(double y) const {
    const std::vector<RowInfo> &rows = design.rows;
    const auto above = std::ranges::lower_bound(rows, y, {}, &RowInfo::coordinate);
    const auto k = static_cast<std::size_t>(above - rows.begin());
    if (k == rows.size()) {
        return rows.size() - 1;
    }
    if (k > 0 && y - rows[k - 1].coordinate < rows[k].coordinate - y) {
        return k - 1;
    }
    return k;
}

bool RippleLegalizer::Impl::isStacked(std::size_t bottomRow, std::size_t rowsSpanned) const {
    const std::vector<RowInfo> &rows = design.rows;
    if (bottomRow + rowsSpanned > rows.size()) {
        return false;
    }
    for (std::size_t k = bottomRow + 1; k < bottomRow + rowsSpanned; ++k) {
        if (std::abs(rows[k].coordinate - (rows[k - 1].coordinate + rows[k - 1].height)) > eps) {
            return false;
        }
    }
    return true;
}

double RippleLegalizer::Impl::snapUp(double x, std::size_t row) const {
    const double pitch = rowPitch[row];
    return rowOrigin[row] + std::ceil((x - rowOrigin[row]) / pitch - eps) * pitch;
}

double RippleLegalizer::Impl::snapDown(double x, std::size_t row) const {
    const double pitch = rowPitch[row];
    return rowOrigin[row] + std::floor((x - rowOrigin[row]) / pitch + eps) * pitch;
}

void RippleLegalizer::Impl::buildPieces() {
    const std::vector<RowInfo> &rows = design.rows;
    pieces.assign(rows.size(), {});
    for (std::size_t k = 0; k < rows.size(); ++k) {
        const double y0 = rows[k].coordinate;
        const double y1 = y0 + rows[k].height;
        std::vector<Interval> cuts;
        for (const BBox &b : blocked) {
            if (b.upperRight.y > y0 + eps && b.lowerLeft.y < y1 - eps) {
                cuts.push_back(b.getX());
            }
        }
        std::ranges::stable_sort(cuts);
        for (const Subrow &sub : rows[k].subrows) {
            double cursor = sub.x.start;
            const auto emit = [&](double lo, double hi) {
                if (hi > lo + eps) {
                    pieces[k].push_back(Piece{.x = Interval(lo, hi), .region = sub.region});
                }
            };
            for (const Interval &cut : cuts) {
                if (cut.end <= cursor + eps || cut.start >= sub.x.end - eps) {
                    continue;
                }
                emit(cursor, std::min(cut.start, sub.x.end));
                cursor = std::max(cursor, cut.end);
            }
            emit(cursor, sub.x.end);
        }
        std::ranges::stable_sort(pieces[k], {}, [](const Piece &p) {
            return p.x.start;
        });
    }
}

void RippleLegalizer::Impl::placeTallCells(std::vector<std::uint32_t> tall) {
    const std::vector<RowInfo> &rows = design.rows;
    std::ranges::stable_sort(tall, [&](std::uint32_t a, std::uint32_t b) {
        const Cell &ca = design.cells[a];
        const Cell &cb = design.cells[b];
        return ca.width * ca.height > cb.width * cb.height;
    });
    for (const std::uint32_t v : tall) {
        const Cell &cell = design.cells[v];
        double bestCost = inf;
        BBox best = BBox(Point(0.0, 0.0), Point(0.0, 0.0));
        for (std::size_t r = 0; r < rows.size(); ++r) {
            const double dy = rows[r].coordinate - gy[v];
            if (std::abs(dy) >= bestCost || !isStacked(r, spanOf[v])) {
                continue;
            }
            std::vector<Interval> free;
            for (const Subrow &sub : rows[r].subrows) {
                if (sub.region == cell.region) {
                    free.push_back(sub.x);
                }
            }
            for (std::size_t k = r + 1; k < r + spanOf[v]; ++k) {
                std::vector<Interval> next;
                for (const Interval &a : free) {
                    for (const Subrow &sub : rows[k].subrows) {
                        const double lo = std::max(a.start, sub.x.start);
                        const double hi = std::min(a.end, sub.x.end);
                        if (sub.region == cell.region && hi > lo) {
                            next.emplace_back(lo, hi);
                        }
                    }
                }
                free = std::move(next);
            }
            const double y0 = rows[r].coordinate;
            const double y1 = y0 + cell.height;
            for (const BBox &b : blocked) {
                if (b.upperRight.y <= y0 + eps || b.lowerLeft.y >= y1 - eps) {
                    continue;
                }
                std::vector<Interval> cut;
                for (const Interval &a : free) {
                    if (b.upperRight.x <= a.start || b.lowerLeft.x >= a.end) {
                        cut.push_back(a);
                        continue;
                    }
                    if (b.lowerLeft.x > a.start) {
                        cut.emplace_back(a.start, b.lowerLeft.x);
                    }
                    if (b.upperRight.x < a.end) {
                        cut.emplace_back(b.upperRight.x, a.end);
                    }
                }
                free = std::move(cut);
            }
            for (const Interval &a : free) {
                const double lo = snapUp(a.start, r);
                const double hi = snapDown(a.end - cell.width, r);
                if (hi < lo - eps) {
                    continue;
                }
                const double x = std::clamp(snapDown(gx[v] + rowPitch[r] / 2.0, r), lo, hi);
                const double cost = std::abs(x - gx[v]) + std::abs(dy);
                if (cost < bestCost) {
                    bestCost = cost;
                    best = BBox(Point(x, y0), Point(x + cell.width, y1));
                }
            }
        }
        if (!std::isfinite(bestCost)) {
            continue;
        }
        xs[v] = best.lowerLeft.x;
        ys[v] = best.lowerLeft.y;
        placed[v] = 1;
        blocked.push_back(best);
    }
}

RippleLegalizer::Impl::Insertion RippleLegalizer::Impl::findInsertion(std::uint32_t t,
                                                                      double halfWidth,
                                                                      std::size_t halfRows) const {
    const std::vector<RowInfo> &rows = design.rows;
    const Cell &target = design.cells[t];
    const double w = target.width;
    const std::size_t h = spanOf[t];
    Insertion best;
    if (h > rows.size()) {
        return best;
    }
    const std::size_t home = getNearestRow(gy[t]);
    const std::size_t lowest = home > halfRows ? home - halfRows : 0;
    const std::size_t highest = std::min(rows.size() - h, home + halfRows);
    if (lowest > highest) {
        return best;
    }
    const std::size_t rowsLo = lowest;
    const std::size_t rowsHi = highest + h - 1;
    const double winLo = gx[t] - halfWidth;
    const double winHi = gx[t] + w + halfWidth;

    std::vector<std::uint32_t> local;
    std::map<std::uint32_t, std::size_t> localOf;
    std::vector<std::vector<Interval>> walls(rowsHi - rowsLo + 1);
    for (std::size_t k = rowsLo; k <= rowsHi; ++k) {
        const std::vector<std::uint32_t> &list = rowCells[k];
        auto it = std::ranges::lower_bound(list, winLo - widest, {}, [&](std::uint32_t c) {
            return xs[c];
        });
        for (; it != list.end() && xs[*it] < winHi; ++it) {
            const std::uint32_t c = *it;
            const double cw = design.cells[c].width;
            if (xs[c] + cw <= winLo) {
                continue;
            }
            const bool inside = xs[c] >= winLo - eps && xs[c] + cw <= winHi + eps &&
                                bottomOf[c] >= rowsLo && bottomOf[c] + spanOf[c] - 1 <= rowsHi;
            if (inside) {
                if (!localOf.contains(c)) {
                    localOf.emplace(c, local.size());
                    local.push_back(c);
                }
            } else {
                walls[k - rowsLo].emplace_back(xs[c], xs[c] + cw);
            }
        }
    }
    const std::size_t n = local.size();
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
        return xs[local[a]] < xs[local[b]];
    });

    struct WindowPiece {
        Interval x;
        int region = noRegion;
        std::vector<std::size_t> members;
    };
    std::vector<std::vector<WindowPiece>> windowPieces(rowsHi - rowsLo + 1);
    for (std::size_t k = rowsLo; k <= rowsHi; ++k) {
        std::vector<Interval> &cuts = walls[k - rowsLo];
        std::ranges::stable_sort(cuts);
        for (const Piece &piece : pieces[k]) {
            const double lo = std::max(piece.x.start, winLo);
            const double hi = std::min(piece.x.end, winHi);
            double cursor = lo;
            const auto emit = [&](double a, double b) {
                if (b > a + eps) {
                    windowPieces[k - rowsLo].push_back(
                        WindowPiece{.x = Interval(a, b), .region = piece.region, .members = {}});
                }
            };
            for (const Interval &cut : cuts) {
                if (cut.end <= cursor + eps || cut.start >= hi - eps) {
                    continue;
                }
                emit(cursor, std::min(cut.start, hi));
                cursor = std::max(cursor, cut.end);
            }
            emit(cursor, hi);
        }
    }
    const auto pieceAt = [&](std::size_t k, double x) -> std::size_t {
        const std::vector<WindowPiece> &list = windowPieces[k - rowsLo];
        for (std::size_t p = 0; p < list.size(); ++p) {
            if (x >= list[p].x.start - eps && x <= list[p].x.end + eps) {
                return p;
            }
        }
        return none;
    };
    std::vector<std::vector<std::pair<std::size_t, std::size_t>>> pieceOf(n);
    for (const std::size_t i : order) {
        const std::uint32_t c = local[i];
        for (std::size_t k = bottomOf[c]; k < bottomOf[c] + spanOf[c]; ++k) {
            const std::size_t p = pieceAt(k, xs[c] + design.cells[c].width / 2.0);
            if (p == none) {
                return best;
            }
            windowPieces[k - rowsLo][p].members.push_back(i);
            pieceOf[i].emplace_back(k, p);
        }
    }
    std::vector<std::vector<std::size_t>> prevOf(n);
    std::vector<std::vector<std::size_t>> nextOf(n);
    for (std::size_t k = rowsLo; k <= rowsHi; ++k) {
        for (const WindowPiece &piece : windowPieces[k - rowsLo]) {
            for (std::size_t m = 0; m < piece.members.size(); ++m) {
                const std::size_t i = piece.members[m];
                if (m > 0) {
                    prevOf[i].push_back(piece.members[m - 1]);
                }
                if (m + 1 < piece.members.size()) {
                    nextOf[i].push_back(piece.members[m + 1]);
                }
            }
        }
    }
    const auto widthOf = [&](std::size_t i) {
        return design.cells[local[i]].width;
    };
    std::vector<double> earliest(n, -inf);
    std::vector<double> latest(n, inf);
    for (const std::size_t i : order) {
        for (const auto &[k, p] : pieceOf[i]) {
            earliest[i] = std::max(earliest[i], snapUp(windowPieces[k - rowsLo][p].x.start, k));
        }
        for (const std::size_t q : prevOf[i]) {
            earliest[i] = std::max(earliest[i], earliest[q] + widthOf(q));
        }
    }
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const std::size_t i = *it;
        for (const auto &[k, p] : pieceOf[i]) {
            latest[i] =
                std::min(latest[i], snapDown(windowPieces[k - rowsLo][p].x.end - widthOf(i), k));
        }
        for (const std::size_t q : nextOf[i]) {
            latest[i] = std::min(latest[i], latest[q] - widthOf(i));
        }
    }

    std::vector<double> towardRight(n);
    std::vector<double> towardLeft(n);
    for (std::size_t bottom = lowest; bottom <= highest; ++bottom) {
        if (opts.powerRailAlignment && h % 2 == 0 && bottom % 2 != 0) {
            continue;
        }
        if (!isStacked(bottom, h)) {
            continue;
        }
        std::vector<double> marks{winLo, winHi};
        for (std::size_t k = bottom; k < bottom + h; ++k) {
            for (const WindowPiece &piece : windowPieces[k - rowsLo]) {
                marks.push_back(piece.x.start);
                marks.push_back(piece.x.end);
                for (const std::size_t i : piece.members) {
                    marks.push_back(xs[local[i]] + widthOf(i) / 2.0);
                }
            }
        }
        std::ranges::sort(marks);
        marks.erase(std::unique(marks.begin(), marks.end()), marks.end());
        const double dy = std::abs(rows[bottom].coordinate - gy[t]);
        for (std::size_t m = 0; m + 1 < marks.size(); ++m) {
            if (marks[m + 1] - marks[m] < eps) {
                continue;
            }
            const double split = (marks[m] + marks[m + 1]) / 2.0;
            double lo = -inf;
            double hi = inf;
            std::vector<std::size_t> lefts;
            std::vector<std::size_t> rights;
            bool valid = true;
            for (std::size_t k = bottom; k < bottom + h && valid; ++k) {
                const std::size_t p = pieceAt(k, split);
                if (p == none || windowPieces[k - rowsLo][p].region != target.region) {
                    valid = false;
                    break;
                }
                const WindowPiece &piece = windowPieces[k - rowsLo][p];
                std::size_t before = none;
                std::size_t after = none;
                for (const std::size_t i : piece.members) {
                    if (xs[local[i]] + widthOf(i) / 2.0 < split) {
                        before = i;
                    } else if (after == none) {
                        after = i;
                    }
                }
                lo = std::max(lo, before == none ? snapUp(piece.x.start, k)
                                                 : earliest[before] + widthOf(before));
                hi = std::min(hi, after == none ? snapDown(piece.x.end - w, k) : latest[after] - w);
                if (before != none) {
                    lefts.push_back(before);
                }
                if (after != none) {
                    rights.push_back(after);
                }
            }
            if (!valid || lo > hi + eps) {
                continue;
            }
            std::ranges::fill(towardRight, -inf);
            std::ranges::fill(towardLeft, -inf);
            for (const std::size_t i : rights) {
                towardRight[i] = std::max(towardRight[i], w);
            }
            for (const std::size_t i : order) {
                if (towardRight[i] == -inf) {
                    continue;
                }
                for (const std::size_t q : nextOf[i]) {
                    towardRight[q] = std::max(towardRight[q], towardRight[i] + widthOf(i));
                }
            }
            for (const std::size_t i : lefts) {
                towardLeft[i] = std::max(towardLeft[i], widthOf(i));
            }
            for (auto it = order.rbegin(); it != order.rend(); ++it) {
                const std::size_t i = *it;
                if (towardLeft[i] == -inf) {
                    continue;
                }
                for (const std::size_t q : prevOf[i]) {
                    towardLeft[q] = std::max(towardLeft[q], towardLeft[i] + widthOf(q));
                }
            }
            const auto evaluate = [&](double x) {
                double cost = std::abs(x - gx[t]);
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint32_t c = local[i];
                    if (towardRight[i] != -inf) {
                        cost += std::abs(std::max(xs[c], x + towardRight[i]) - gx[c]) -
                                std::abs(xs[c] - gx[c]);
                    } else if (towardLeft[i] != -inf) {
                        cost += std::abs(std::min(xs[c], x - towardLeft[i]) - gx[c]) -
                                std::abs(xs[c] - gx[c]);
                    }
                }
                return cost;
            };
            std::vector<std::pair<double, double>> breaks{{gx[t], 2.0}};
            double slope = -1.0;
            for (std::size_t i = 0; i < n; ++i) {
                const std::uint32_t c = local[i];
                if (towardRight[i] != -inf) {
                    const double start = xs[c] - towardRight[i];
                    if (gx[c] <= xs[c]) {
                        breaks.emplace_back(start, 1.0);
                    } else {
                        breaks.emplace_back(start, -1.0);
                        breaks.emplace_back(gx[c] - towardRight[i], 2.0);
                    }
                } else if (towardLeft[i] != -inf) {
                    slope -= 1.0;
                    const double end = xs[c] + towardLeft[i];
                    if (gx[c] >= xs[c]) {
                        breaks.emplace_back(end, 1.0);
                    } else {
                        breaks.emplace_back(gx[c] + towardLeft[i], 2.0);
                        breaks.emplace_back(end, -1.0);
                    }
                }
            }
            std::ranges::sort(breaks);
            double position = lo;
            double value = evaluate(lo);
            double bestValue = value;
            double bestX = lo;
            std::size_t b = 0;
            while (b < breaks.size() && breaks[b].first <= lo) {
                slope += breaks[b].second;
                ++b;
            }
            for (; b < breaks.size() && breaks[b].first < hi; ++b) {
                value += slope * (breaks[b].first - position);
                position = breaks[b].first;
                slope += breaks[b].second;
                if (value < bestValue - eps) {
                    bestValue = value;
                    bestX = position;
                }
            }
            value += slope * (hi - position);
            if (value < bestValue - eps) {
                bestX = hi;
            }
            const std::size_t row = bottom;
            const double down = std::clamp(snapDown(bestX, row), lo, hi);
            const double up = std::clamp(snapUp(bestX, row), lo, hi);
            const double costDown = evaluate(down);
            const double costUp = evaluate(up);
            const double x = costUp < costDown - eps ? up : down;
            const double total = std::min(costDown, costUp) + dy;
            if (total < best.cost - eps) {
                best.cost = total;
                best.bottomRow = bottom;
                best.x = x;
                best.moved.clear();
                for (std::size_t i = 0; i < n; ++i) {
                    const std::uint32_t c = local[i];
                    if (towardRight[i] != -inf && x + towardRight[i] > xs[c] + eps) {
                        best.moved.emplace_back(c, x + towardRight[i]);
                    } else if (towardLeft[i] != -inf && x - towardLeft[i] < xs[c] - eps) {
                        best.moved.emplace_back(c, x - towardLeft[i]);
                    }
                }
            }
        }
    }
    return best;
}

void RippleLegalizer::Impl::commit(std::uint32_t t, const Insertion &best) {
    for (const auto &[c, x] : best.moved) {
        xs[c] = x;
    }
    xs[t] = best.x;
    ys[t] = design.rows[best.bottomRow].coordinate;
    bottomOf[t] = best.bottomRow;
    placed[t] = 1;
    for (std::size_t k = best.bottomRow; k < best.bottomRow + spanOf[t]; ++k) {
        std::vector<std::uint32_t> &list = rowCells[k];
        const auto at = std::ranges::lower_bound(list, xs[t], {}, [&](std::uint32_t c) {
            return xs[c];
        });
        list.insert(at, t);
    }
}

void RippleLegalizer::Impl::legalizeCells(std::vector<std::uint32_t> cells, Stats &stats) {
    std::ranges::stable_sort(cells, [&](std::uint32_t a, std::uint32_t b) {
        return gx[a] < gx[b];
    });
    const std::size_t frameInterval =
        design.onFrame && opts.frames > 0 ? (cells.size() + opts.frames - 1) / opts.frames : 0;
    std::size_t visited = 0;
    for (const std::uint32_t t : cells) {
        double halfWidth = opts.windowWidth * rowHeight;
        std::size_t halfRows = opts.windowRows;
        bool done = false;
        for (std::size_t attempt = 0; attempt <= opts.windowGrowth && !done; ++attempt) {
            const Insertion best = findInsertion(t, halfWidth, halfRows);
            if (std::isfinite(best.cost)) {
                commit(t, best);
                done = true;
                stats.windowGrowths += attempt;
            }
            halfWidth *= 2.0;
            halfRows *= 2;
        }
        stats.unplaced += done ? 0 : 1;
        ++visited;
        if (frameInterval > 0 && visited % frameInterval == 0) {
            writeFrame("Inserting cells (MGL)", visited);
        }
    }
}

std::size_t RippleLegalizer::Impl::getSlot(std::size_t row, std::uint32_t cell) const {
    const std::vector<std::uint32_t> &list = rowCells[row];
    auto it = std::ranges::lower_bound(list, xs[cell] - eps, {}, [&](std::uint32_t c) {
        return xs[c];
    });
    while (it != list.end() && *it != cell) {
        ++it;
    }
    return static_cast<std::size_t>(it - list.begin());
}

std::size_t RippleLegalizer::Impl::matchDisplacement() {
    std::map<std::tuple<double, double, int>, std::vector<std::uint32_t>> types;
    for (std::uint32_t c = 0; c < design.cells.size(); ++c) {
        if (small[c] != 0 && placed[c] != 0) {
            types[{design.cells[c].width, design.cells[c].height, design.cells[c].region}]
                .push_back(c);
        }
    }
    const auto getDelta = [&](std::uint32_t cell, std::uint32_t slot) {
        return (std::abs(xs[slot] - gx[cell]) + std::abs(ys[slot] - gy[cell])) / rowHeight;
    };
    const double bin = 8.0 * rowHeight;
    const auto nx =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(design.die.getWidth() / bin)));
    const auto ny =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(design.die.getHeight() / bin)));
    const auto binOf = [&](double x, double y) {
        const auto bx = std::min(
            nx - 1, static_cast<std::size_t>(std::max(0.0, (x - design.die.lowerLeft.x) / bin)));
        const auto by = std::min(
            ny - 1, static_cast<std::size_t>(std::max(0.0, (y - design.die.lowerLeft.y) / bin)));
        return std::pair(bx, by);
    };
    std::size_t moved = 0;
    for (auto &[key, group] : types) {
        if (group.size() < 2) {
            continue;
        }
        std::ranges::stable_sort(group, [&](std::uint32_t a, std::uint32_t b) {
            return getDelta(a, a) > getDelta(b, b);
        });
        std::map<std::pair<std::size_t, std::size_t>, std::vector<std::size_t>> binned;
        for (std::size_t k = 0; k < group.size(); ++k) {
            binned[binOf(xs[group[k]], ys[group[k]])].push_back(k);
        }
        std::vector<char> taken(group.size(), 0);
        for (std::size_t seed = 0; seed < group.size(); ++seed) {
            if (taken[seed] != 0) {
                continue;
            }
            std::vector<std::uint32_t> members{group[seed]};
            taken[seed] = 1;
            const auto [cx, cy] = binOf(gx[group[seed]], gy[group[seed]]);
            const auto visit = [&](long bx, long by) {
                if (bx < 0 || by < 0 || bx >= static_cast<long>(nx) ||
                    by >= static_cast<long>(ny)) {
                    return;
                }
                const auto found =
                    binned.find({static_cast<std::size_t>(bx), static_cast<std::size_t>(by)});
                if (found == binned.end()) {
                    return;
                }
                for (const std::size_t k : found->second) {
                    if (members.size() < opts.matchingGroup && taken[k] == 0) {
                        taken[k] = 1;
                        members.push_back(group[k]);
                    }
                }
            };
            const auto sx = static_cast<long>(cx);
            const auto sy = static_cast<long>(cy);
            for (long ring = 0; ring <= 16 && members.size() < opts.matchingGroup; ++ring) {
                if (ring == 0) {
                    visit(sx, sy);
                    continue;
                }
                for (long d = -ring; d <= ring; ++d) {
                    visit(sx + d, sy - ring);
                    visit(sx + d, sy + ring);
                }
                for (long d = -ring + 1; d <= ring - 1; ++d) {
                    visit(sx - ring, sy + d);
                    visit(sx + ring, sy + d);
                }
            }
            const std::size_t k = members.size();
            if (k < 2) {
                continue;
            }
            std::vector<double> cost(k * k);
            for (std::size_t i = 0; i < k; ++i) {
                for (std::size_t j = 0; j < k; ++j) {
                    cost[i * k + j] =
                        getPhi(getDelta(members[i], members[j]), opts.toleratedDisplacement);
                }
            }
            const std::vector<std::size_t> assignment = solveAssignment(cost, k);
            double before = 0.0;
            double after = 0.0;
            for (std::size_t i = 0; i < k; ++i) {
                before += cost[i * k + i];
                after += cost[i * k + assignment[i]];
            }
            if (after >= before - eps) {
                continue;
            }
            std::vector<double> nx2(k);
            std::vector<double> ny2(k);
            std::vector<std::size_t> nb(k);
            std::vector<std::vector<std::pair<std::size_t, std::size_t>>> slots(k);
            for (std::size_t j = 0; j < k; ++j) {
                const std::uint32_t c = members[j];
                for (std::size_t r = bottomOf[c]; r < bottomOf[c] + spanOf[c]; ++r) {
                    slots[j].emplace_back(r, getSlot(r, c));
                }
            }
            for (std::size_t i = 0; i < k; ++i) {
                const std::uint32_t slot = members[assignment[i]];
                nx2[i] = xs[slot];
                ny2[i] = ys[slot];
                nb[i] = bottomOf[slot];
            }
            for (std::size_t i = 0; i < k; ++i) {
                const std::uint32_t c = members[i];
                moved += assignment[i] != i ? 1 : 0;
                for (const auto &[r, s] : slots[assignment[i]]) {
                    rowCells[r][s] = c;
                }
            }
            for (std::size_t i = 0; i < k; ++i) {
                const std::uint32_t c = members[i];
                xs[c] = nx2[i];
                ys[c] = ny2[i];
                bottomOf[c] = nb[i];
            }
        }
    }
    return moved;
}

bool RippleLegalizer::Impl::refineFixedOrder() {
    std::vector<std::size_t> nodeOf(design.cells.size(), 0);
    std::vector<std::uint32_t> cellOf{none};
    for (std::uint32_t c = 0; c < design.cells.size(); ++c) {
        if (small[c] != 0 && placed[c] != 0) {
            nodeOf[c] = cellOf.size();
            cellOf.push_back(c);
        }
    }
    if (cellOf.size() < 2) {
        return true;
    }
    NetworkSimplex flow(cellOf.size());
    std::vector<std::pair<std::uint32_t, std::uint32_t>> neighbours;
    std::vector<std::tuple<std::uint32_t, double, double>> ranges;
    for (std::size_t node = 1; node < cellOf.size(); ++node) {
        const std::uint32_t c = cellOf[node];
        const double target = std::round((gx[c] - rowOrigin[bottomOf[c]]) / rowPitch[bottomOf[c]]) *
                                  rowPitch[bottomOf[c]] +
                              rowOrigin[bottomOf[c]];
        flow.addArc(node, 0, 1.0, target);
        flow.addArc(0, node, 1.0, -target);
    }
    for (std::size_t k = 0; k < rowCells.size(); ++k) {
        const std::vector<std::uint32_t> &list = rowCells[k];
        std::size_t p = 0;
        for (std::size_t m = 0; m < list.size(); ++m) {
            const std::uint32_t c = list[m];
            const double width = design.cells[c].width;
            while (p < pieces[k].size() && pieces[k][p].x.end < xs[c] + width - eps) {
                ++p;
            }
            if (p == pieces[k].size()) {
                return false;
            }
            const Piece &piece = pieces[k][p];
            const bool first = m == 0 || xs[list[m - 1]] < piece.x.start - eps;
            const bool last = m + 1 == list.size() || xs[list[m + 1]] >= piece.x.end - eps;
            const double lo = snapUp(piece.x.start, k);
            const double hi = snapDown(piece.x.end - width, k);
            ranges.emplace_back(c, lo, hi);
            if (first) {
                flow.addArc(0, nodeOf[c], unlimited, -lo);
            } else {
                const std::uint32_t left = list[m - 1];
                neighbours.emplace_back(left, c);
                flow.addArc(nodeOf[left], nodeOf[c], unlimited, -design.cells[left].width);
            }
            if (last) {
                flow.addArc(nodeOf[c], 0, unlimited, hi);
            }
        }
    }
    if (!flow.solve(200 * cellOf.size() + 1000000)) {
        ktlog.warning(
            "RippleLG: the fixed-order flow did not finish; keeping the matched placement");
        return false;
    }
    const std::vector<double> &potential = flow.getPotentials();
    std::vector<double> next = xs;
    for (std::size_t node = 1; node < cellOf.size(); ++node) {
        const std::uint32_t c = cellOf[node];
        const std::size_t row = bottomOf[c];
        const double x = potential[0] - potential[node];
        next[c] = rowOrigin[row] + std::round((x - rowOrigin[row]) / rowPitch[row]) * rowPitch[row];
    }
    const bool legal =
        std::ranges::all_of(neighbours,
                            [&](const auto &pair) {
                                return next[pair.first] + design.cells[pair.first].width <=
                                       next[pair.second] + eps;
                            }) &&
        std::ranges::all_of(ranges, [&](const auto &range) {
            const auto &[c, lo, hi] = range;
            return next[c] >= lo - eps && next[c] <= hi + eps;
        });
    if (!legal) {
        ktlog.warning(
            "RippleLG: the fixed-order solution is not legal; keeping the matched placement");
        return false;
    }
    xs = std::move(next);
    return true;
}

double RippleLegalizer::Impl::getHpwl() const {
    return getNetHpwl(
        design.nets.size(),
        [this](std::size_t n) {
            return design.nets[n];
        },
        [this](const NetPin &pin) {
            return Point(xs[pin.cellId] + pin.offset.x, ys[pin.cellId] + pin.offset.y);
        });
}

void RippleLegalizer::Impl::recordStage(const std::string &name, double seconds,
                                        Stats &stats) const {
    StageResult result{.name = name,
                       .displacement = 0.0,
                       .maxDisplacement = 0.0,
                       .hpwl = getHpwl(),
                       .seconds = seconds};
    for (std::uint32_t c = 0; c < design.cells.size(); ++c) {
        if (design.cells[c].isMovable) {
            const double moved = std::abs(xs[c] - gx[c]) + std::abs(ys[c] - gy[c]);
            result.displacement += moved;
            result.maxDisplacement = std::max(result.maxDisplacement, moved);
        }
    }
    stats.stages.push_back(result);
}

void RippleLegalizer::Impl::writeFrame(const std::string &note, std::size_t step) const {
    if (!design.onFrame) {
        return;
    }
    std::vector<float> fx(xs.size());
    std::vector<float> fy(ys.size());
    for (std::size_t c = 0; c < xs.size(); ++c) {
        fx[c] = static_cast<float>(xs[c]);
        fy[c] = static_cast<float>(ys[c]);
    }
    FrameInfo info;
    info.note = note;
    info.step = step;
    info.numSteps = xs.size();
    info.hpwl = getHpwl();
    info.mandatory = true;
    design.onFrame(fx, fy, info);
}

Stats RippleLegalizer::Impl::run(const RipplelgOptions &options) {
    ScopedTimer timer("ripplelg");
    opts = options;
    Stats stats;
    const std::vector<RowInfo> &rows = design.rows;
    rowHeight = inf;
    for (const RowInfo &row : rows) {
        if (row.height > 0.0 && !row.subrows.empty()) {
            rowHeight = std::min(rowHeight, row.height);
        }
    }
    const std::size_t n = design.cells.size();
    xs.resize(n);
    ys.resize(n);
    gx.resize(n);
    gy.resize(n);
    spanOf.assign(n, 1);
    bottomOf.assign(n, 0);
    small.assign(n, 0);
    placed.assign(n, 0);
    for (std::size_t c = 0; c < n; ++c) {
        xs[c] = gx[c] = design.cells[c].position.x;
        ys[c] = gy[c] = design.cells[c].position.y;
    }
    stats.hpwlBefore = getHpwl();
    if (rows.empty() || !std::isfinite(rowHeight)) {
        return stats;
    }
    rowOrigin.assign(rows.size(), 0.0);
    rowPitch.assign(rows.size(), 1.0);
    for (std::size_t k = 0; k < rows.size(); ++k) {
        rowOrigin[k] = rows[k].subrows.empty() ? 0.0 : rows[k].subrows.front().x.start;
        rowPitch[k] = rows[k].getPitch() > 0.0 ? rows[k].getPitch() : 1.0;
    }
    blocked = design.blockages;
    std::vector<std::uint32_t> tall;
    std::vector<std::uint32_t> cells;
    for (std::uint32_t c = 0; c < n; ++c) {
        const Cell &cell = design.cells[c];
        if (!cell.isMovable) {
            continue;
        }
        ++stats.movable;
        spanOf[c] = getSpan(cell.height);
        stats.multiRow += spanOf[c] > 1 ? 1 : 0;
        if (spanOf[c] > opts.maxCellRows) {
            tall.push_back(c);
        } else {
            small[c] = 1;
            cells.push_back(c);
            widest = std::max(widest, cell.width);
        }
    }
    stats.tall = tall.size();
    writeFrame("Input - global placement", 0);
    const auto clock = [] {
        return std::chrono::steady_clock::now();
    };
    auto start = clock();
    placeTallCells(tall);
    buildPieces();
    rowCells.assign(rows.size(), {});
    legalizeCells(cells, stats);
    recordStage("multi-row global legalization",
                std::chrono::duration<double>(clock() - start).count(), stats);
    writeFrame("After MGL", 1);
    if (opts.matching) {
        start = clock();
        stats.matched = matchDisplacement();
        recordStage("max displacement matching",
                    std::chrono::duration<double>(clock() - start).count(), stats);
        writeFrame("After matching", 2);
    }
    if (opts.fixedOrder) {
        start = clock();
        refineFixedOrder();
        recordStage("fixed row and order (min-cost flow)",
                    std::chrono::duration<double>(clock() - start).count(), stats);
    }
    writeFrame("Legal placement", 3);
    for (std::size_t c = 0; c < n; ++c) {
        if (design.cells[c].isMovable) {
            design.cells[c].position = Point(xs[c], ys[c]);
        }
    }
    return stats;
}

RippleLegalizer::RippleLegalizer(Design &design) : pImpl(std::make_unique<Impl>(design)) {}

RippleLegalizer::~RippleLegalizer() = default;

RippleLegalizer::RippleLegalizer(RippleLegalizer &&) noexcept = default;

RippleLegalizer &RippleLegalizer::operator=(RippleLegalizer &&) noexcept = default;

void RippleLegalizer::legalize(const RipplelgOptions &options) {
    report(pImpl->run(options));
}

}  // namespace ktplace::ripplelg
