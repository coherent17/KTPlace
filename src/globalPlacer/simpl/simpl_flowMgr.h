#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/simpl/simpl_dm.h"

namespace ktplace::simpl {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] simplDM &dm() noexcept {
        return simplDb;
    }

    SimplResult run();
    [[nodiscard]] simplSolution solution() const;

private:
    simplDM simplDb;
};

}  // namespace ktplace::simpl
