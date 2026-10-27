// @file kt_solutionMgr.h
// What each stage hands back to the flow: every cell's position, indexed by cell id.

#pragma once

#include "geometry/kt_geometry.h"

#include <vector>

namespace ktplace {

struct simplSolution {
    std::vector<Point> positions;
};

/// What NTUPlace1 hands back: a complete placement, like the legalizers'. It is
/// overfull on purpose -- the paper's global placement is, and legalization is a
/// separate step.
struct ntuplace1Solution {
    std::vector<Point> positions;
};

/// What a legalizer hands back: where every cell ended up, indexed by cell id.
struct abacusSolution {
    std::vector<Point> positions;
};

/// What the multi-row legalizer hands back. Same shape as abacusSolution: both
/// are a complete placement, and a flow treats them the same way.
struct multiRowSolution {
    std::vector<Point> positions;
};

struct tetrisSolution {
    std::vector<Point> positions;
};

/// What a detailed placer hands back.
struct fastdpSolution {
    std::vector<Point> positions;
};

}  // namespace ktplace
