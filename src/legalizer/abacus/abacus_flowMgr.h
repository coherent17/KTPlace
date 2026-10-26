#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/abacus/abacus_dm.h"

namespace ktplace::abacus {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] abacusDM &getDm() noexcept {
        return abacusDb;
    }

    void run();
    [[nodiscard]] abacusSolution getSolution() const;

private:
    abacusDM abacusDb;
};

}  // namespace ktplace::abacus
