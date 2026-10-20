#include "macroPlacer/dmp/dmp_flowMgr.h"

namespace ktplace::dmp {

flowMgr::flowMgr(const ktDM &db) : dmpDb(buildDmpDM(db)) {}

DmpResult flowMgr::run() {
    return dmpDb.run();
}

dmpSolution flowMgr::solution() const {
    return dmpDb.solution();
}

}  // namespace ktplace::dmp
