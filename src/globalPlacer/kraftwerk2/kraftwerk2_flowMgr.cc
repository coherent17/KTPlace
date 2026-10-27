#include "globalPlacer/kraftwerk2/kraftwerk2_flowMgr.h"

namespace ktplace::kraftwerk2 {

flowMgr::flowMgr(const ktDM &db) : kraftwerk2Db(buildKraftwerk2DM(db)) {}

void flowMgr::run() {
    kraftwerk2Db.run();
}

kraftwerk2Solution flowMgr::getSolution() const {
    return kraftwerk2Db.getSolution();
}

}  // namespace ktplace::kraftwerk2
