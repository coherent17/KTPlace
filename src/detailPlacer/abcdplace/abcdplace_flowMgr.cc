#include "detailPlacer/abcdplace/abcdplace_flowMgr.h"

namespace ktplace::abcdplace {

flowMgr::flowMgr(const ktDM &db) : abcdplaceDb(buildAbcdplaceDM(db)) {}

void flowMgr::run() {
    abcdplaceDb.run();
}

abcdplaceSolution flowMgr::getSolution() const {
    return abcdplaceDb.getSolution();
}

}  // namespace ktplace::abcdplace
