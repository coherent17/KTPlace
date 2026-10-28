#include "legalizer/ripplelg/ripplelg_flowMgr.h"

namespace ktplace::ripplelg {

flowMgr::flowMgr(const ktDM &db) : ripplelgDb(buildRipplelgDM(db)) {}

void flowMgr::run() {
    ripplelgDb.run();
}

ripplelgSolution flowMgr::getSolution() const {
    return ripplelgDb.getSolution();
}

}  // namespace ktplace::ripplelg
