#include "legalizer/tetris/tetris_flowMgr.h"

namespace ktplace::tetris {

flowMgr::flowMgr(const ktDM &db) : tetrisDb(buildTetrisDM(db)) {}

void flowMgr::run() {
    tetrisDb.run();
}

tetrisSolution flowMgr::getSolution() const {
    return tetrisDb.getSolution();
}

}  // namespace ktplace::tetris
