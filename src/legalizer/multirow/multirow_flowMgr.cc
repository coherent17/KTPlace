#include "legalizer/multirow/multirow_flowMgr.h"

namespace ktplace::multirow {

flowMgr::flowMgr(const ktDM &db) : multirowDb(buildMultiRowDM(db)) {}

MultiRowResult flowMgr::run() {
    return multirowDb.run();
}

multiRowSolution flowMgr::solution() const {
    return multirowDb.solution();
}

}  // namespace ktplace::multirow
