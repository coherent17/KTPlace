// @file simpl_dm.h
// SimPL's own database.
//
// SimPL does not read ktDM and does not write to it. It reads a simpl::Design
// and keeps its own working state here, so a run leaves the shared database
// untouched until the flow commits the answer.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "placer/simpl/simpl_design.h"
#include "placer/simpl/simpl_options.h"
#include "placer/simpl/simpl_placer.h"
#include "visualization/kt_plotOptions.h"

namespace ktplace {

class ktDM;

namespace simpl {

/// SimPL's design plus the state of the current run.
class simplDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit simplDM(Design design);

    // SimPL's options, read from the run's option store, and the parameters they
    // configure. Built and reported here rather than by the flow, which does not
    // know what SimPL has options.
    [[nodiscard]] SimplOptions options() const;
    [[nodiscard]] SimplParams params() const noexcept {
        return par;
    }

    // Whether placement regions are honoured. False means `add_option simpl fences 0`.
    [[nodiscard]] bool useFences() const noexcept {
        return useFences_;
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

    /// Run global placement and return where every cell ended up. The solver's
    /// statistics are reported from inside the run; they are not returned, because
    /// nothing outside the solver can act on them. Nothing is written outside this
    /// object.
    simplSolution run(const SimplParams &params, bool useFences);

    friend simplDM buildSimplDM(const ktDM &db, const PlotOptions &plot);

private:
    Design d;
    SimplParams par;
    bool useFences_ = true;
};

/// ktDM -> SimPL's own database.
///
/// The whole of SimPL's contact with the shared database, in one place. It
/// returns a plain value: SimPL keeps no reference back to ktDM, so a stale
/// design cannot be mistaken for a current one.
///
/// `plotDir` is where the host should write snapshots when SimPL asks for one.
/// SimPL itself never draws; it hands the positions to the sink installed here.
[[nodiscard]] simplDM buildSimplDM(const ktDM &db, const PlotOptions &plot);

}  // namespace simpl
}  // namespace ktplace
