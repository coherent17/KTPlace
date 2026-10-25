#include "detailPlacer/fastdp/fastdp_flowMgr.h"

namespace ktplace::fastdp {

flowMgr::flowMgr(const ktDM &db) : fastdpDb(buildFastdpDM(db)) {}

void flowMgr::run() {
    fastdpDb.run();
}

fastdpSolution flowMgr::getSolution() const {
    return fastdpDb.getSolution();
}

}  // namespace ktplace::fastdp
