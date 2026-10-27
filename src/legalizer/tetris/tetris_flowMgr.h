#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/tetris/tetris_dm.h"

namespace ktplace::tetris {

class flowMgr {
public:
    explicit flowMgr(const ktDM &db);

    [[nodiscard]] tetrisDM &getDm() noexcept {
        return tetrisDb;
    }

    void run();
    [[nodiscard]] tetrisSolution getSolution() const;

private:
    tetrisDM tetrisDb;
};

}  // namespace ktplace::tetris
