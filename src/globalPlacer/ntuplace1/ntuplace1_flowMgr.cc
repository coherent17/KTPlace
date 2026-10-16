#include "globalPlacer/ntuplace1/ntuplace1_flowMgr.h"

namespace ktplace::ntuplace1 {

flowMgr::flowMgr(const ktDM &db) : ntuplace1Db(buildNtuplace1DM(db)) {}

Ntuplace1Result flowMgr::run() {
    return ntuplace1Db.run();
}

ntuplace1Solution flowMgr::solution() const {
    return ntuplace1Db.solution();
}

}  // namespace ktplace::ntuplace1
