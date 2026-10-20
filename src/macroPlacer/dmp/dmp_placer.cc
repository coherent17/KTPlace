#include "macroPlacer/dmp/dmp_placer.h"

#include "datamodel/kt_graph.h"
#include "datamodel/kt_macroPlan.h"
#include "macroPlacer/dmp/dmp_dm.h"
#include "macroPlacer/dmp/dmp_options.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

namespace ktplace::dmp {

namespace {

struct Box {
    double xlo = std::numeric_limits<double>::max();
    double xhi = std::numeric_limits<double>::lowest();
    double ylo = std::numeric_limits<double>::max();
    double yhi = std::numeric_limits<double>::lowest();

    void add(double x, double y) {
        xlo = std::min(xlo, x);
        xhi = std::max(xhi, x);
        ylo = std::min(ylo, y);
        yhi = std::max(yhi, y);
    }
    [[nodiscard]] bool empty() const {
        return xlo > xhi;
    }
};

struct NetSpan {
    Box rest;
    Box own;
};

struct Axis {
    double lo = 0.0;
    double hi = 0.0;
    double ownLo = 0.0;
    double ownHi = 0.0;
};

struct Candidate {
    double at = 0.0;
    double cost = 0.0;
};

double pinX(const Design &design, const Pin &pin) {
    return pin.macro == noMacro ? pin.x
                                : design.macros[static_cast<std::size_t>(pin.macro)].x + pin.x;
}

double pinY(const Design &design, const Pin &pin) {
    return pin.macro == noMacro ? pin.y
                                : design.macros[static_cast<std::size_t>(pin.macro)].y + pin.y;
}

double axisCost(const std::vector<Axis> &axes, double at, double size, bool flip) {
    double total = 0.0;
    for (const Axis &a : axes) {
        const double ownLo = flip ? size - a.ownHi : a.ownLo;
        const double ownHi = flip ? size - a.ownLo : a.ownHi;
        total += std::max(a.hi, at + ownHi) - std::min(a.lo, at + ownLo);
    }
    return total;
}

double axisOptimum(const std::vector<Axis> &axes, double size, bool flip) {
    std::vector<double> points;
    points.reserve(2 * axes.size());
    for (const Axis &a : axes) {
        const double ownLo = flip ? size - a.ownHi : a.ownLo;
        const double ownHi = flip ? size - a.ownLo : a.ownHi;
        points.push_back(a.hi - ownHi);
        points.push_back(a.lo - ownLo);
    }
    if (points.empty()) {
        return 0.0;
    }
    const auto middle = points.begin() + static_cast<std::ptrdiff_t>(points.size() / 2);
    std::ranges::nth_element(points, middle);
    return *middle;
}

class MacroPlacer {
public:
    MacroPlacer(Design &design, const DmpOptions &options) : d(design), opts(options) {
        macroNets.resize(d.macros.size());
        macroPins.resize(d.macros.size());
        for (std::size_t p = 0; p < d.pins.size(); ++p) {
            if (d.pins[p].macro != noMacro) {
                macroPins[static_cast<std::size_t>(d.pins[p].macro)].push_back(p);
            }
        }
        for (std::size_t n = 0; n < d.nets.size(); ++n) {
            for (const std::size_t p : d.nets[n].pins) {
                const int m = d.pins[p].macro;
                if (m != noMacro && (macroNets[static_cast<std::size_t>(m)].empty() ||
                                     macroNets[static_cast<std::size_t>(m)].back() != n)) {
                    macroNets[static_cast<std::size_t>(m)].push_back(n);
                }
            }
        }
    }

    std::size_t pass() {
        std::size_t improved = opts.swap ? swapPass() : 0;
        for (std::size_t m = 0; m < d.macros.size(); ++m) {
            if (d.macros[m].movable && improve(m)) {
                ++improved;
            }
        }
        return improved;
    }

private:
    Design &d;
    const DmpOptions &opts;
    std::vector<std::vector<std::size_t>> macroNets;
    std::vector<std::vector<std::size_t>> macroPins;

    [[nodiscard]] bool legal(std::size_t m, double x, double y) const {
        const Macro &a = d.macros[m];
        if (x < d.die[0] || y < d.die[1] || x + a.width > d.die[2] || y + a.height > d.die[3]) {
            return false;
        }
        if (std::abs(x - a.originX) + std::abs(y - a.originY) > d.rules.maxDisplacement) {
            return false;
        }
        const double spacing = d.rules.minSpacing;
        for (std::size_t o = 0; o < d.macros.size(); ++o) {
            if (o == m) {
                continue;
            }
            const Macro &b = d.macros[o];
            const double gapX = std::max(b.x - (x + a.width), x - (b.x + b.width));
            const double gapY = std::max(b.y - (y + a.height), y - (b.y + b.height));
            if ((gapY < 0.0 && gapX < spacing) || (gapX < 0.0 && gapY < spacing)) {
                return false;
            }
        }
        return true;
    }

    void spans(std::size_t m, std::vector<Axis> &xs, std::vector<Axis> &ys) const {
        for (const std::size_t n : macroNets[m]) {
            NetSpan span;
            for (const std::size_t p : d.nets[n].pins) {
                const Pin &pin = d.pins[p];
                if (pin.macro == static_cast<int>(m)) {
                    span.own.add(pin.x, pin.y);
                } else {
                    span.rest.add(pinX(d, pin), pinY(d, pin));
                }
            }
            if (span.rest.empty()) {
                continue;
            }
            xs.push_back({span.rest.xlo, span.rest.xhi, span.own.xlo, span.own.xhi});
            ys.push_back({span.rest.ylo, span.rest.yhi, span.own.ylo, span.own.yhi});
        }
    }

    [[nodiscard]] std::vector<double> positions(std::size_t m, bool horizontal,
                                                double optimum) const {
        const Macro &a = d.macros[m];
        const double origin = horizontal ? a.originX : a.originY;
        const double current = horizontal ? a.x : a.y;
        const double size = horizontal ? a.width : a.height;
        const double reach = d.rules.maxDisplacement;
        const double lo = std::ceil(std::max(horizontal ? d.die[0] : d.die[1], origin - reach));
        const double hi =
            std::floor(std::min((horizontal ? d.die[2] : d.die[3]) - size, origin + reach));
        std::vector<double> out{current};
        if (!opts.shift || lo > hi) {
            return out;
        }
        const auto keep = [&out, lo, hi](double at) {
            out.push_back(std::clamp(std::round(at), lo, hi));
        };
        keep(origin);
        keep(optimum);
        const std::size_t steps = std::max<std::size_t>(opts.gridSteps, 1);
        for (std::size_t k = 0; k <= steps; ++k) {
            keep(lo + (hi - lo) * static_cast<double>(k) / static_cast<double>(steps));
        }
        const double spacing = d.rules.minSpacing;
        for (std::size_t o = 0; o < d.macros.size(); ++o) {
            if (o == m) {
                continue;
            }
            const Macro &b = d.macros[o];
            const double bLo = horizontal ? b.x : b.y;
            const double bSize = horizontal ? b.width : b.height;
            const double otherLo = horizontal ? b.y : b.x;
            const double otherSize = horizontal ? b.height : b.width;
            const double ownOther = horizontal ? a.originY : a.originX;
            const double ownOtherSize = horizontal ? a.height : a.width;
            if (otherLo >= ownOther + reach + ownOtherSize ||
                otherLo + otherSize <= ownOther - reach) {
                continue;
            }
            for (const double at : {bLo - size - spacing, bLo + bSize + spacing}) {
                if (at >= lo && at <= hi) {
                    out.push_back(at);
                }
            }
        }
        if (opts.snap) {
            out = snapped(out, horizontal, size, lo, hi);
        }
        std::ranges::sort(out);
        const auto [first, last] = std::ranges::unique(out);
        out.erase(first, last);
        return out;
    }

    [[nodiscard]] std::vector<double> snapped(const std::vector<double> &at, bool horizontal,
                                              double size, double lo, double hi) const {
        const double origin = horizontal ? d.grid.originX : d.grid.originY;
        const double pitch = horizontal ? d.grid.pitchX : d.grid.pitchY;
        if (pitch <= 0.0) {
            return at;
        }
        std::vector<double> out;
        if (onGrid(at.front(), size, horizontal)) {
            out.push_back(at.front());
        }
        for (const double value : at) {
            for (const double edge : {value, value + size}) {
                const double below = origin + std::floor((edge - origin) / pitch) * pitch;
                for (const double line : {below, below + pitch}) {
                    const double candidate = edge == value ? line : line - size;
                    if (candidate >= lo && candidate <= hi) {
                        out.push_back(candidate);
                    }
                }
            }
        }
        return out.empty() ? at : out;
    }

    [[nodiscard]] bool onGrid(double lo, double size, bool horizontal) const {
        const double origin = horizontal ? d.grid.originX : d.grid.originY;
        const double pitch = horizontal ? d.grid.pitchX : d.grid.pitchY;
        if (pitch <= 0.0) {
            return true;
        }
        const auto aligned = [origin, pitch](double edge) {
            return std::fmod(edge - origin, pitch) == 0.0;
        };
        return aligned(lo) || aligned(lo + size);
    }

    [[nodiscard]] double towards(double value, double target, bool horizontal) const {
        const double origin = horizontal ? d.grid.originX : d.grid.originY;
        const double pitch = horizontal ? d.grid.pitchX : d.grid.pitchY;
        if (!opts.snap || pitch <= 0.0) {
            return value;
        }
        const double steps = (value - origin) / pitch;
        return origin + (value > target ? std::floor(steps) : std::ceil(steps)) * pitch;
    }

    [[nodiscard]] std::vector<Candidate> rank(const std::vector<double> &at,
                                              const std::vector<Axis> &axes, double size,
                                              bool flip) const {
        std::vector<Candidate> out;
        out.reserve(at.size());
        for (const double value : at) {
            out.push_back({value, axisCost(axes, value, size, flip)});
        }
        std::ranges::stable_sort(out, {}, &Candidate::cost);
        return out;
    }

    bool improve(std::size_t m) {
        Macro &a = d.macros[m];
        std::vector<Axis> xs;
        std::vector<Axis> ys;
        spans(m, xs, ys);
        if (xs.empty()) {
            return false;
        }
        const bool forced =
            opts.snap && (!onGrid(a.x, a.width, true) || !onGrid(a.y, a.height, false));
        const double current =
            forced ? std::numeric_limits<double>::max()
                   : axisCost(xs, a.x, a.width, false) + axisCost(ys, a.y, a.height, false);
        double best = current;
        double bestX = a.x;
        double bestY = a.y;
        bool bestFlipX = false;
        bool bestFlipY = false;
        const double reach = d.rules.maxDisplacement;
        for (const bool flipX : {false, true}) {
            if (flipX && !opts.flip) {
                continue;
            }
            const double optimumX = axisOptimum(xs, a.width, flipX);
            const std::vector<Candidate> byX =
                rank(positions(m, true, optimumX), xs, a.width, flipX);
            for (const bool flipY : {false, true}) {
                if (flipY && !opts.flip) {
                    continue;
                }
                const double optimumY = axisOptimum(ys, a.height, flipY);
                const std::vector<Candidate> byY =
                    rank(positions(m, false, optimumY), ys, a.height, flipY);
                const auto consider = [&](double x, double y, double cost) {
                    if (cost < best - 0.5 && legal(m, x, y)) {
                        best = cost;
                        bestX = x;
                        bestY = y;
                        bestFlipX = flipX;
                        bestFlipY = flipY;
                    }
                };
                for (const Candidate &cx : byX) {
                    if (cx.cost + byY.front().cost >= best - 0.5) {
                        break;
                    }
                    for (const Candidate &cy : byY) {
                        if (cx.cost + cy.cost >= best - 0.5) {
                            break;
                        }
                        if (std::abs(cx.at - a.originX) + std::abs(cy.at - a.originY) <= reach) {
                            consider(cx.at, cy.at, cx.cost + cy.cost);
                        }
                    }
                }
                if (!opts.shift || reach <= 0.0) {
                    continue;
                }
                const double signX = optimumX >= a.originX ? 1.0 : -1.0;
                const double signY = optimumY >= a.originY ? 1.0 : -1.0;
                const std::size_t steps = std::max<std::size_t>(opts.gridSteps, 1);
                for (std::size_t k = 0; k <= steps; ++k) {
                    const double along =
                        std::floor(reach * static_cast<double>(k) / static_cast<double>(steps));
                    const double x = towards(a.originX + signX * along, a.originX, true);
                    const double y = towards(a.originY + signY * (reach - along), a.originY, false);
                    consider(x, y,
                             axisCost(xs, x, a.width, flipX) + axisCost(ys, y, a.height, flipY));
                }
            }
        }
        if (opts.shiftGain > 0.0 && (bestX != a.x || bestY != a.y)) {
            const double here =
                axisCost(xs, a.x, a.width, false) + axisCost(ys, a.y, a.height, false);
            double stay = here;
            bool stayFlipX = false;
            bool stayFlipY = false;
            for (const bool flipX : {false, true}) {
                for (const bool flipY : {false, true}) {
                    if ((flipX || flipY) && !opts.flip) {
                        continue;
                    }
                    const double cost =
                        axisCost(xs, a.x, a.width, flipX) + axisCost(ys, a.y, a.height, flipY);
                    if (cost < stay - 0.5) {
                        stay = cost;
                        stayFlipX = flipX;
                        stayFlipY = flipY;
                    }
                }
            }
            if (stay - best < opts.shiftGain * here) {
                best = stay;
                bestX = a.x;
                bestY = a.y;
                bestFlipX = stayFlipX;
                bestFlipY = stayFlipY;
            }
        }
        if (best >= current - 0.5) {
            return false;
        }
        a.x = bestX;
        a.y = bestY;
        flip(m, bestFlipX, bestFlipY);
        return true;
    }

    void flip(std::size_t m, bool flipX, bool flipY) {
        Macro &a = d.macros[m];
        a.orient = orientFromMirrors(mirrorsX(a.orient) != flipX, mirrorsY(a.orient) != flipY);
        for (const std::size_t p : macroPins[m]) {
            Pin &pin = d.pins[p];
            if (flipX) {
                pin.x = a.width - pin.x;
            }
            if (flipY) {
                pin.y = a.height - pin.y;
            }
        }
    }

    [[nodiscard]] double netsCost(const std::vector<std::size_t> &nets) const {
        double total = 0.0;
        for (const std::size_t n : nets) {
            Box box;
            for (const std::size_t p : d.nets[n].pins) {
                box.add(pinX(d, d.pins[p]), pinY(d, d.pins[p]));
            }
            total += (box.xhi - box.xlo) + (box.yhi - box.ylo);
        }
        return total;
    }

    void bestFlip(std::size_t m) {
        if (!opts.flip) {
            return;
        }
        const Macro &a = d.macros[m];
        std::vector<Axis> xs;
        std::vector<Axis> ys;
        spans(m, xs, ys);
        const bool flipX = axisCost(xs, a.x, a.width, true) < axisCost(xs, a.x, a.width, false);
        const bool flipY = axisCost(ys, a.y, a.height, true) < axisCost(ys, a.y, a.height, false);
        flip(m, flipX, flipY);
    }

    [[nodiscard]] bool reaches(std::size_t m, double x, double y) const {
        const Macro &a = d.macros[m];
        return std::abs(x - a.originX) + std::abs(y - a.originY) <= d.rules.maxDisplacement;
    }

    bool trySwap(std::size_t i, std::size_t j) {
        Macro &a = d.macros[i];
        Macro &b = d.macros[j];
        std::vector<std::size_t> nets;
        std::ranges::set_union(macroNets[i], macroNets[j], std::back_inserter(nets));
        const double before = netsCost(nets);
        const Macro savedA = a;
        const Macro savedB = b;
        std::swap(a.x, b.x);
        std::swap(a.y, b.y);
        bestFlip(i);
        bestFlip(j);
        if (netsCost(nets) < before - 0.5) {
            return true;
        }
        flip(i, mirrorsX(a.orient) != mirrorsX(savedA.orient),
             mirrorsY(a.orient) != mirrorsY(savedA.orient));
        flip(j, mirrorsX(b.orient) != mirrorsX(savedB.orient),
             mirrorsY(b.orient) != mirrorsY(savedB.orient));
        a = savedA;
        b = savedB;
        return false;
    }

    std::size_t swapPass() {
        std::size_t swapped = 0;
        for (std::size_t i = 0; i < d.macros.size(); ++i) {
            for (std::size_t j = i + 1; j < d.macros.size(); ++j) {
                const Macro &a = d.macros[i];
                const Macro &b = d.macros[j];
                if (!a.movable || !b.movable || a.width != b.width || a.height != b.height ||
                    (a.x == b.x && a.y == b.y) || !reaches(i, b.x, b.y) || !reaches(j, a.x, a.y)) {
                    continue;
                }
                swapped += trySwap(i, j) ? 1 : 0;
            }
        }
        return swapped;
    }
};

}  // namespace

double macroNetHpwl(const Design &design) {
    double total = 0.0;
    for (const Net &net : design.nets) {
        Box box;
        for (const std::size_t p : net.pins) {
            box.add(pinX(design, design.pins[p]), pinY(design, design.pins[p]));
        }
        if (!box.empty()) {
            total += (box.xhi - box.xlo) + (box.yhi - box.ylo);
        }
    }
    return total;
}

DmpResult placeMacros(Design &design, const DmpOptions &options) {
    const ScopedTimer timer("dmp");
    DmpResult result;
    result.hpwlBefore = macroNetHpwl(design);
    MacroPlacer placer(design, options);
    for (std::size_t pass = 0; pass < options.passes; ++pass) {
        ++result.passes;
        if (placer.pass() == 0) {
            break;
        }
    }
    result.hpwlAfter = macroNetHpwl(design);
    for (const Macro &macro : design.macros) {
        result.moved += macro.x != macro.originX || macro.y != macro.originY ? 1 : 0;
        result.flipped += macro.orient != macro.originOrient ? 1 : 0;
    }
    result.seconds = timer.elapsedSeconds();
    return result;
}

}  // namespace ktplace::dmp
