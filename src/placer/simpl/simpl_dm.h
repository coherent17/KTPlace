// @file simpl_dm.h
// SimPL's own database.
//
// SimPL does not read ktDM and does not write to it. It reads a simpl::Design
// and keeps its own working state here, so a run leaves the shared database
// untouched until the flow commits the answer.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "placer/simpl/simpl_design.h"
#include "placer/simpl/simpl_placer.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ktplace::simpl {

/// SimPL's design plus the state of the current run.
class simplDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit simplDM(Design design);

    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }
    [[nodiscard]] Design &design() noexcept {
        return d;
    }

    /// Install the host's frame hook. SimPL never draws on its own.
    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    /// Run global placement and return where every cell ended up. The solver's
    /// statistics are reported from inside the run; they are not returned, because
    /// nothing outside the solver can act on them. Nothing is written outside this
    /// object.
    simplSolution run(const SimplParams &params, const std::string &plotDir, bool useFences);

private:
    Design d;
};

}  // namespace ktplace::simpl
