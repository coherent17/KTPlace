// @file abacus_dm.h
// Abacus's own database.
//
// Abacus does not read ktDM and does not write to it. It runs against an
// abacus::Design -- rows, cells, and just enough netlist to report the
// wirelength legalization cost -- and keeps its answer here.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/abacus/abacus_design.h"
#include "legalizer/abacus/abacus_options.h"


namespace ktplace {

class ktDM;

namespace abacus {

/// Abacus's design plus the state of the current run.
class abacusDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit abacusDM(Design design);

    [[nodiscard]] const AbacusOptions &options() const noexcept {
        return opts;
    }

    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }
    [[nodiscard]] Design &design() noexcept {
        return d;
    }

    /// Install the host's frame hook. Abacus never draws on its own.
    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    /// Legalize, and return where every cell ended up. The pass's statistics
    /// are reported from inside the run; they are not returned, because nothing
    /// outside Abacus can act on them.
    abacusSolution run();

    AbacusOptions opts;

private:
    Design d;
};

/// ktDM -> abacus::abacusDM.
///
/// Only what this stage reads: the cells and their sizes, the rows, the regions, and just enough
/// netlist to report the wirelength legalization costs. The stage itself never
/// draws: it hands the positions to the visualizer, which decides where they go.
[[nodiscard]] abacusDM buildAbacusDM(const ktDM &db);

}  // namespace abacus
}  // namespace ktplace
