// @file multirow_dm.h
// The multi-row legalizer's own database.
//
// It does not read ktDM and does not write to it. It runs against a
// multirow::Design -- cells, rows, regions, and just enough netlist to report
// the wirelength the pass cost -- and hands its answer back from here.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_design.h"
#include "legalizer/multirow/multirow_legalizer.h"

#include <string>

namespace ktplace::multirow {

class multirowDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit multirowDM(Design design);

    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }
    [[nodiscard]] Design &design() noexcept {
        return d;
    }

    /// Install the host's frame hook. This pass never draws on its own.
    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    /// Legalize, and return where every cell ended up. The pass's statistics are
    /// reported from inside the run; they are not returned, because nothing
    /// outside this pass can act on them.
    multiRowSolution run(const MultiRowLegalizeParams &params);

private:
    Design d;
};

}  // namespace ktplace::multirow
