#include "detailPlacer/fastdp/fastdp_flowMgr.h"

namespace ktplace::fastdp {

flowMgr::flowMgr(const ktDM &db) : fastdpDb(buildFastdpDM(db)) {}

FastdpResult flowMgr::run() {
    return fastdpDb.run();
}

fastdpSolution flowMgr::solution() const {
    return fastdpDb.solution();
}

}  // namespace ktplace::fastdp
