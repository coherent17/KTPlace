// @file fastdp_flowMgr.h
// Runs one FastDP detailed placement.
//
// This is the stage as the rest of KTPlace sees it: run against a database the
// caller owns, hand back the placement. It knows about reporting because that is
// a flow concern; it does not know about ktDM, which is the adaptor's job.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_dm.h"

namespace ktplace::fastdp {

class flowMgr {
public:
    explicit flowMgr(fastdpDM &db) : dm(db) {}

    /// Place, and return where every cell ended up. Reports the pass's statistics
    /// from inside the run and returns only the placement, so the flow has one
    /// thing to do with the result: commit it.
    fastdpSolution runDetailedPlacer(const DetailPlaceParams &params);

private:
    fastdpDM &dm;
};

}  // namespace ktplace::fastdp
