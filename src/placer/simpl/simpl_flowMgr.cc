#include "placer/simpl/simpl_flowMgr.h"

namespace ktplace::simpl {

simplSolution flowMgr::runGlobalPlacer() {
    return dm.run();
}

}  // namespace ktplace::simpl
