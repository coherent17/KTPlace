// @file kt_solutionMgr.h
// The one mutable part of a design, and the two views a stage works from.

#pragma once

#include "datamodel/kt_graph.h"

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace ktplace {

/// Half-perimeter wirelength of a placement, computed from a netlist and the
/// positions that go with it, through netHpwl (kt_hpwl.h) like every stage's HPWL.
///
/// @p x and @p y are indexed by vertex id and must cover every vertex. Entries
/// for net vertices are ignored.
[[nodiscard]] double netlistHPWL(const Graph &graph, const std::vector<double> &x,
                                 const std::vector<double> &y);

struct simplSolution {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// What NTUPlace1 hands back: a complete placement, like the legalizers'. It is
/// overfull on purpose -- the paper's global placement is, and legalization is a
/// separate step.
struct ntuplace1Solution {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// What a legalizer hands back: where every cell ended up, indexed by cell id.
struct abacusSolution {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// What the multi-row legalizer hands back. Same shape as abacusSolution: both
/// are a complete placement, and a flow treats them the same way.
struct multiRowSolution {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// What a detailed placer hands back.
struct fastdpSolution {
    std::vector<double> xs;
    std::vector<double> ys;
};

/// A placement a stage produced and ktDM will accept: the positions of every
/// vertex, indexed by vertex id.
///
/// This is what a stage produces and what a stage consumes, and the only part of
/// a design that changes between stages. Everything else about a vertex -- its
/// name, type, width, height, whether it is fixed, which net it is on -- is
/// fixed once parsed.
///
/// It is a value type, not a view of the design. A stage builds one, moves cells
/// around in it, and hands it back; nothing writes to the design until the flow
/// calls ktDM::setPlacementSolution(). That is the whole point: a stage cannot
/// leave the design half-updated, and a stage can be tested against a
/// solutionMgr without a ktDM at all.
class solutionMgr {
public:
    solutionMgr() = default;

    /// Sized for a graph of @p numVertices vertices. Vertices are indexed by id,
    /// so this is getNumVertices(), not the number of cells -- nets occupy ids
    /// too, and leaving them zero is what keeps the indexing identical to the
    /// graph's.
    explicit solutionMgr(std::size_t numVertices) {
        resize(numVertices);
    }

    /// Resize, zero-filling. The positions of a design being read are the design's
    /// own; this is how a Solution is seeded from them.
    void resize(std::size_t numVertices) {
        xPos.assign(numVertices, 0.0);
        yPos.assign(numVertices, 0.0);
    }

    /// The positions currently in the design, for a graph of this shape.
    [[nodiscard]] static solutionMgr fromGraph(const Graph &graph);

    /// Write these positions into @p graph's vertices.
    ///
    /// The only path from a stage back to the design. A vertex that is not a cell
    /// keeps the net data it already had: a Solution carries no net information
    /// and must not clear any.
    void commitTo(Graph &graph) const;

    [[nodiscard]] double x(std::size_t vertexId) const {
        return xPos.at(vertexId);
    }
    [[nodiscard]] double y(std::size_t vertexId) const {
        return yPos.at(vertexId);
    }
    void setX(std::size_t vertexId, double x) {
        xPos.at(vertexId) = x;
    }
    void setY(std::size_t vertexId, double y) {
        yPos.at(vertexId) = y;
    }

    /// Take both coordinates for every cell at once. `xs` and `ys` must be
    /// indexed by cell id and the same length, which is the shape stages keep
    /// their results in.
    void setAll(const std::vector<double> &xs, const std::vector<double> &ys) {
        if (xs.size() != ys.size() || xs.size() != xPos.size()) {
            throw std::invalid_argument("solutionMgr::setAll: size mismatch");
        }
        xPos = xs;
        yPos = ys;
    }

    [[nodiscard]] const std::vector<double> &xs() const {
        return xPos;
    }
    [[nodiscard]] const std::vector<double> &ys() const {
        return yPos;
    }

    [[nodiscard]] std::size_t size() const {
        return xPos.size();
    }

private:
    std::vector<double> xPos;
    std::vector<double> yPos;
};

}  // namespace ktplace
