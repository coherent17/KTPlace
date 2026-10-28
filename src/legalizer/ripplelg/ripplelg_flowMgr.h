#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/ripplelg/ripplelg_dm.h"

namespace ktplace::ripplelg {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] ripplelgDM &getDm() noexcept {
        return ripplelgDb;
    }

    void run();
    [[nodiscard]] ripplelgSolution getSolution() const;

private:
    ripplelgDM ripplelgDb;
};

}  // namespace ktplace::ripplelg
