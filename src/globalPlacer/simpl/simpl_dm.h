// @file simpl_dm.h
// SimPL's own database.
//
// SimPL does not read ktDM and does not write to it. It reads a simpl::Design
// and keeps its own working state here, so a run leaves the shared database
// untouched until the flow commits the answer.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/simpl/simpl_design.h"
#include "globalPlacer/simpl/simpl_options.h"

namespace ktplace {

class ktDM;

namespace simpl {

/// SimPL's design plus the state of the current run.
class simplDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit simplDM(Design design);

    [[nodiscard]] const SimplOptions &options() const noexcept {
        return opts;
    }
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
    void setBoundsHook(BoundsHook hook) {
        d.onBounds = std::move(hook);
    }

    /// Run global placement and return where every cell ended up. The solver's
    /// statistics are reported from inside the run; they are not returned, because
    /// nothing outside the solver can act on them. Nothing is written outside this
    /// object.
    simplSolution run();

    friend simplDM buildSimplDM(const ktDM &db);

private:
    Design d;
    SimplOptions opts;
};

/// ktDM -> SimPL's own database.
///
/// The whole of SimPL's contact with the shared database, in one place. It
/// returns a plain value: SimPL keeps no reference back to ktDM, so a stale
/// design cannot be mistaken for a current one.
[[nodiscard]] simplDM buildSimplDM(const ktDM &db);

}  // namespace simpl
}  // namespace ktplace
