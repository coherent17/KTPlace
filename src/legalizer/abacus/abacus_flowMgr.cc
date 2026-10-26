#include "legalizer/abacus/abacus_flowMgr.h"

namespace ktplace::abacus {

flowMgr::flowMgr(const ktDM &db) : abacusDb(buildAbacusDM(db)) {}

void flowMgr::run() {
    abacusDb.run();
}

abacusSolution flowMgr::getSolution() const {
    return abacusDb.getSolution();
}

}  // namespace ktplace::abacus
