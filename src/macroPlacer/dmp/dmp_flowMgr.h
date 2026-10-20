#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "macroPlacer/dmp/dmp_dm.h"

namespace ktplace::dmp {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] dmpDM &dm() noexcept {
        return dmpDb;
    }

    DmpResult run();
    [[nodiscard]] dmpSolution solution() const;

private:
    dmpDM dmpDb;
};

}  // namespace ktplace::dmp
