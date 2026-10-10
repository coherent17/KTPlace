// @file multirow_flowMgr.h
// Runs one multi-row legalization.
//
// This is the stage as the rest of KTPlace sees it: run against a database the
// caller owns, hand back the placement. It knows about reporting because that is
// a flow concern; it does not know about ktDM, which is the adaptor's job.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_dm.h"

namespace ktplace::multirow {

class flowMgr {
public:
    explicit flowMgr(multirowDM &db) : dm(db) {}

    /// Legalize, and return where every cell ended up. Reports the pass's
    /// statistics from inside the run and returns only the placement, so the
    /// flow has one thing to do with the result: commit it.
    multiRowSolution runLegalizer(const MultiRowLegalizeParams &params);

private:
    multirowDM &dm;
};

}  // namespace ktplace::multirow
