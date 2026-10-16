#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_dm.h"

namespace ktplace::multirow {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] multirowDM &dm() noexcept {
        return multirowDb;
    }

    MultiRowResult run();
    [[nodiscard]] multiRowSolution solution() const;

private:
    multirowDM multirowDb;
};

}  // namespace ktplace::multirow
