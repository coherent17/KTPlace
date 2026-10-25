#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_dm.h"

namespace ktplace::fastdp {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] fastdpDM &getDm() noexcept {
        return fastdpDb;
    }

    void run();
    [[nodiscard]] fastdpSolution getSolution() const;

private:
    fastdpDM fastdpDb;
};

}  // namespace ktplace::fastdp
