#include "placer/simpl/simpl_dm.h"

#include "util/kt_scopedTimer.h"

namespace ktplace::simpl {
simplDM::simplDM(Design design) : d(std::move(design)) {}

simplSolution simplDM::run(const SimplParams &params, const std::string &plotDir, bool useFences) {
    ScopedTimer timer("simpl");

    simplPlacer placer(d);
    const SimplResult result = placer.place(params, plotDir, useFences);
    reportSimpl(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}

}  // namespace ktplace::simpl
