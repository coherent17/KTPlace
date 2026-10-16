// @file ntuplace1_dm.h
// NTUPlace1's own database.
//
// It does not read ktDM and does not write to it. It runs against an
// ntuplace1::Design copied out of the shared database, and hands its answer back
// from here.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/ntuplace1/ntuplace1_design.h"
#include "globalPlacer/ntuplace1/ntuplace1_options.h"

namespace ktplace {

class ktDM;

namespace ntuplace1 {

class ntuplace1DM {
public:
    explicit ntuplace1DM(Design design);

    [[nodiscard]] const Ntuplace1Options &options() const noexcept {
        return opts;
    }

    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }
    [[nodiscard]] Design &design() noexcept {
        return d;
    }

    [[nodiscard]] ntuplace1Solution solution() const;

    Ntuplace1Options opts;

private:
    Design d;
};

[[nodiscard]] ntuplace1DM buildNtuplace1DM(const ktDM &db);

}  // namespace ntuplace1
}  // namespace ktplace
