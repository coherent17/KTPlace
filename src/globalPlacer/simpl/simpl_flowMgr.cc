#include "globalPlacer/simpl/simpl_flowMgr.h"

namespace ktplace::simpl {

flowMgr::flowMgr(const ktDM &db) : simplDb(buildSimplDM(db)) {}

SimplResult flowMgr::run() {
    return simplDb.run();
}

simplSolution flowMgr::getSolution() const {
    return simplDb.getSolution();
}

}  // namespace ktplace::simpl
