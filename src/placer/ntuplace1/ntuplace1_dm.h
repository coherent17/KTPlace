// @file ntuplace1_dm.h
// NTUPlace1's own database.
//
// Unlike SimPL, NTUPlace1 has no private working state: it partitions the design
// it is given and writes the answer straight back. So this is a handle on the
// caller's database plus the options that configure the run, not a copy of the
// netlist -- which is what "build the DM" buys this stage.

#pragma once

#include "datamodel/kt_dm.h"
#include "datamodel/kt_solutionMgr.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/ntuplace1/ntuplace1_options.h"

namespace ktplace::ntuplace1 {

class ntuplace1DM {
public:
    explicit ntuplace1DM(ktDM &shared);

    // NTUPlace1's options, read from the run's option store, and the parameters
    // they configure. Assembled here so the flow has nothing to collect.
    [[nodiscard]] Ntuplace1Options options() const;
    [[nodiscard]] RatioPlaceParams params() const;

    // The database the partitioner reads and writes.
    [[nodiscard]] ktDM &database() const noexcept {
        return shared;
    }

    // Where the cells ended up, as the flow's solution type.
    [[nodiscard]] ntuplace1Solution solution() const;

private:
    ktDM &shared;
};

[[nodiscard]] ntuplace1DM buildNtuplace1DM(ktDM &db);

}  // namespace ktplace::ntuplace1
