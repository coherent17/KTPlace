// @file ntuplace1_flowMgr.h
// Runs one NTUPlace1 partition.
//
// This is the stage as the rest of KTPlace sees it: run against a database the
// caller owns, hand back the placement. It knows about reporting because that is a
// flow concern; it does not know about ktDM.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "placer/ntuplace1/ntuplace1_dm.h"

namespace ktplace::ntuplace1 {

class flowMgr {
public:
    explicit flowMgr(ntuplace1DM &db) : dm(db) {}

    // Partition, and return where every cell ended up. The placement handed back
    // is deliberately overfull: the paper's global placement is, and legalization
    // is a separate step in both the paper and this flow.
    ntuplace1Solution runGlobalPlacer(const RatioPlaceParams &params);

private:
    ntuplace1DM &dm;
};

}  // namespace ktplace::ntuplace1
