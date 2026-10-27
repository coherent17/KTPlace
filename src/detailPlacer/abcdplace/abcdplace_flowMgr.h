#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/abcdplace/abcdplace_dm.h"

namespace ktplace::abcdplace {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] abcdplaceDM &getDm() noexcept {
        return abcdplaceDb;
    }

    void run();
    [[nodiscard]] abcdplaceSolution getSolution() const;

private:
    abcdplaceDM abcdplaceDb;
};

}  // namespace ktplace::abcdplace
