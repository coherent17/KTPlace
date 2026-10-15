// @file ntuplace1_flowMgr.cc
// Running one NTUPlace1 partition

#include "placer/ntuplace1/ntuplace1_flowMgr.h"

#include "placer/ntuplace1/kt_ntuplace1.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

namespace ktplace::ntuplace1 {

ntuplace1Solution flowMgr::runGlobalPlacer() {
    ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");

    reportOptions(dm.options());
    RatioPlacer placer(dm.design());
    const RatioPlaceResult result = placer.place(dm.options());
    reportNtuPlace1(result);

    // Only the placement crosses the boundary. The statistics are printed above and
    // dropped here, so the flow only ever sees where the cells went.
    return dm.solution();
}

}  // namespace ktplace::ntuplace1
