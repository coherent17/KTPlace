// @file simpl_flowMgr.h
// Runs one SimPL placement.
//
// This is the stage as the rest of KTPlace sees it: build a simpl::DM, run it,
// hand back the answer. It knows about plotting and timing because those are
// flow concerns; it does not know about ktDM, which is the adaptor's job.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "placer/simpl/simpl_dm.h"

#include <string>

namespace ktplace::simpl {

/// One global-placement run.
class flowMgr {
public:
    /// Runs against a database the caller owns, normally one the adaptor built.
    explicit flowMgr(simplDM &db) : dm(db) {}

    /// Place, and return where every cell ended up. `plotDir` empty disables
    /// frames; `useFences` false measures the design without its fences.
    ///
    /// Reports the solver's statistics from inside the run and returns only the
    /// placement, so the flow has one thing to do with the result: commit it.
    simplSolution runGlobalPlacer(const SimplParams &params, const std::string &plotDir = {},
                                  bool useFences = true);

private:
    simplDM &dm;
};

}  // namespace ktplace::simpl
