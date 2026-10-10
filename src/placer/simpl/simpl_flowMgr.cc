#include "placer/simpl/simpl_flowMgr.h"

namespace ktplace::simpl {

simplSolution flowMgr::runGlobalPlacer(const SimplParams &params, const std::string &plotDir,
                                       bool useFences) {
    return dm.run(params, plotDir, useFences);
}

}  // namespace ktplace::simpl
