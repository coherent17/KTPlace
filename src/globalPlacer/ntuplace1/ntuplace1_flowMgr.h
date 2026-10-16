#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/ntuplace1/ntuplace1_dm.h"

namespace ktplace::ntuplace1 {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] ntuplace1DM &dm() noexcept {
        return ntuplace1Db;
    }

    Ntuplace1Result run();
    [[nodiscard]] ntuplace1Solution solution() const;

private:
    ntuplace1DM ntuplace1Db;
};

}  // namespace ktplace::ntuplace1
