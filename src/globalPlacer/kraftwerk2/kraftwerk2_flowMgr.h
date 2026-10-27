#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_dm.h"

namespace ktplace::kraftwerk2 {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] kraftwerk2DM &getDm() noexcept {
        return kraftwerk2Db;
    }

    void run();
    [[nodiscard]] kraftwerk2Solution getSolution() const;

private:
    kraftwerk2DM kraftwerk2Db;
};

}  // namespace ktplace::kraftwerk2
