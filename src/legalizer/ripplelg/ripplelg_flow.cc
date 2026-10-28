#include "legalizer/ripplelg/ripplelg_flow.h"

#include <algorithm>
#include <cmath>

namespace ktplace::ripplelg {

namespace {

constexpr double tolerance = 1e-7;

}  // namespace

NetworkSimplex::NetworkSimplex(std::size_t nodes)
    : numNodes(nodes),
      root(nodes),
      parent(nodes + 1, nodes),
      parentArc(nodes + 1, 0),
      depth(nodes + 1, 1),
      children(nodes + 1),
      potential(nodes + 1, 0.0) {}

void NetworkSimplex::addArc(std::size_t from, std::size_t to, double capacity, double cost) {
    arcs.push_back(Arc{.from = from, .to = to, .capacity = capacity, .cost = cost, .flow = 0.0});
}

double NetworkSimplex::getReducedCost(std::size_t a) const {
    return arcs[a].cost + potential[arcs[a].from] - potential[arcs[a].to];
}

double NetworkSimplex::getCost() const {
    double total = 0.0;
    for (const Arc &arc : arcs) {
        total += arc.flow * arc.cost;
    }
    return total;
}

void NetworkSimplex::refresh(std::size_t top, double shift) {
    std::vector<std::size_t> stack{top};
    while (!stack.empty()) {
        const std::size_t a = stack.back();
        stack.pop_back();
        depth[a] = depth[parent[a]] + 1;
        potential[a] += shift;
        for (const std::size_t c : children[a]) {
            stack.push_back(c);
        }
    }
}

void NetworkSimplex::reroot(std::size_t newRoot, std::size_t oldTop, std::size_t attachTo,
                            std::size_t arc) {
    std::vector<std::size_t> path;
    for (std::size_t a = newRoot;; a = parent[a]) {
        path.push_back(a);
        if (a == oldTop) {
            break;
        }
    }
    std::vector<std::size_t> &siblings = children[parent[oldTop]];
    siblings.erase(std::ranges::find(siblings, oldTop));
    std::vector<std::size_t> between(path.size(), 0);
    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        between[i] = parentArc[path[i]];
    }
    for (std::size_t i = path.size() - 1; i >= 1; --i) {
        const std::size_t above = path[i];
        const std::size_t below = path[i - 1];
        std::vector<std::size_t> &kids = children[above];
        kids.erase(std::ranges::find(kids, below));
        children[below].push_back(above);
        parent[above] = below;
        parentArc[above] = between[i - 1];
    }
    parent[newRoot] = attachTo;
    parentArc[newRoot] = arc;
    children[attachTo].push_back(newRoot);
}

void NetworkSimplex::pivot(std::size_t entering, bool forward) {
    const Arc &e = arcs[entering];
    const std::size_t s = forward ? e.from : e.to;
    const std::size_t t = forward ? e.to : e.from;
    std::size_t a = t;
    std::size_t b = s;
    while (a != b) {
        if (depth[a] > depth[b]) {
            a = parent[a];
        } else if (depth[b] > depth[a]) {
            b = parent[b];
        } else {
            a = parent[a];
            b = parent[b];
        }
    }
    const std::size_t join = a;

    struct Step {
        std::size_t arc;
        bool along;
        std::size_t child;
        bool sourceSide;
    };
    std::vector<Step> cycle;
    std::vector<Step> sourceSide;
    for (std::size_t n = s; n != join; n = parent[n]) {
        const std::size_t pa = parentArc[n];
        sourceSide.push_back(
            Step{.arc = pa, .along = arcs[pa].from == parent[n], .child = n, .sourceSide = true});
    }
    for (auto it = sourceSide.rbegin(); it != sourceSide.rend(); ++it) {
        cycle.push_back(*it);
    }
    cycle.push_back(Step{.arc = entering, .along = forward, .child = root, .sourceSide = false});
    for (std::size_t n = t; n != join; n = parent[n]) {
        const std::size_t pa = parentArc[n];
        cycle.push_back(
            Step{.arc = pa, .along = arcs[pa].from == n, .child = n, .sourceSide = false});
    }

    double delta = unlimited;
    std::size_t leaving = cycle.size();
    for (std::size_t k = 0; k < cycle.size(); ++k) {
        const Arc &arc = arcs[cycle[k].arc];
        const double room = cycle[k].along ? arc.capacity - arc.flow : arc.flow;
        if (room <= delta) {
            delta = room;
            leaving = k;
        }
    }
    if (!std::isfinite(delta)) {
        delta = 0.0;
    }
    for (const Step &step : cycle) {
        arcs[step.arc].flow += step.along ? delta : -delta;
    }
    const Step &out = cycle[leaving];
    if (out.arc == entering) {
        return;
    }
    const double reduced = getReducedCost(entering);
    if (out.sourceSide) {
        const double shift = forward ? -reduced : reduced;
        reroot(s, out.child, t, entering);
        refresh(s, shift);
    } else {
        const double shift = forward ? reduced : -reduced;
        reroot(t, out.child, s, entering);
        refresh(t, shift);
    }
}

bool NetworkSimplex::solve(std::size_t maxPivots) {
    for (std::size_t u = 0; u < numNodes; ++u) {
        arcs.push_back(Arc{.from = root, .to = u, .capacity = unlimited, .cost = 0.0, .flow = 0.0});
        parent[u] = root;
        parentArc[u] = arcs.size() - 1;
        children[root].push_back(u);
    }
    depth[root] = 0;
    const std::size_t userArcs = arcs.size() - numNodes;
    if (userArcs == 0) {
        return true;
    }
    const std::size_t block = std::max<std::size_t>(
        16, static_cast<std::size_t>(std::sqrt(static_cast<double>(userArcs))));
    std::size_t next = 0;
    for (std::size_t pivots = 0; pivots < maxPivots; ++pivots) {
        double worst = -tolerance;
        std::size_t chosen = userArcs;
        bool chosenForward = true;
        std::size_t scanned = 0;
        while (scanned < userArcs) {
            const std::size_t a = next;
            next = next + 1 == userArcs ? 0 : next + 1;
            ++scanned;
            const Arc &arc = arcs[a];
            const double reduced = getReducedCost(a);
            const double scale = 1.0 + std::abs(arc.cost);
            if (arc.flow < arc.capacity && reduced < worst * scale) {
                worst = reduced / scale;
                chosen = a;
                chosenForward = true;
            } else if (arc.flow > 0.0 && -reduced < worst * scale) {
                worst = -reduced / scale;
                chosen = a;
                chosenForward = false;
            }
            if (chosen != userArcs && scanned % block == 0) {
                break;
            }
        }
        if (chosen == userArcs) {
            return true;
        }
        pivot(chosen, chosenForward);
    }
    return false;
}

}  // namespace ktplace::ripplelg
