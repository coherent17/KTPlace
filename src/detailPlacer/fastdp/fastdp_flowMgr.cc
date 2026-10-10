#include "detailPlacer/fastdp/fastdp_flowMgr.h"

namespace ktplace::fastdp {

fastdpSolution flowMgr::runDetailedPlacer(const DetailPlaceParams &params) {
    return dm.run(params);
}

}  // namespace ktplace::fastdp
