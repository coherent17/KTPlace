#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/abacus/abacus_dm.h"

namespace ktplace::abacus {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] abacusDM &dm() noexcept {
        return abacusDb;
    }

    AbacusResult run();
    [[nodiscard]] abacusSolution solution() const;

private:
    abacusDM abacusDb;
};

}  // namespace ktplace::abacus
