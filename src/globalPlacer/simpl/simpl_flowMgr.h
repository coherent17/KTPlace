#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/simpl/simpl_dm.h"

namespace ktplace::simpl {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] simplDM &getDm() noexcept {
        return simplDb;
    }

    void run();
    [[nodiscard]] simplSolution getSolution() const;

private:
    simplDM simplDb;
};

}  // namespace ktplace::simpl
