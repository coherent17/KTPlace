#include "placer/simpl/simpl_flowMgr.h"

namespace ktplace::simpl {

simplSolution flowMgr::runGlobalPlacer(const SimplParams &params, bool useFences) {
    return dm.run(params, useFences);
}

}  // namespace ktplace::simpl
