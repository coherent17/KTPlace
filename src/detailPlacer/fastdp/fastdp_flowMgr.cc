#include "detailPlacer/fastdp/fastdp_flowMgr.h"

namespace ktplace::fastdp {

fastdpSolution flowMgr::runDetailedPlacer() {
    return dm.run();
}

}  // namespace ktplace::fastdp
