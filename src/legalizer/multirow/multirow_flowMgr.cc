#include "legalizer/multirow/multirow_flowMgr.h"

namespace ktplace::multirow {

flowMgr::flowMgr(const ktDM &db) : multirowDb(buildMultiRowDM(db)) {}

void flowMgr::run() {
    multirowDb.run();
}

multiRowSolution flowMgr::getSolution() const {
    return multirowDb.getSolution();
}

}  // namespace ktplace::multirow
