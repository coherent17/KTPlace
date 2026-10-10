#include "detailPlacer/fastdp/fastdp_dm.h"

#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

namespace ktplace::fastdp {
namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside FastDP can act on these, so they are
/// printed here and not returned.
void reportRun(const DetailPlaceResult &res) {
    ktReportTable table("FastDP detailed placement results");
    table.setHeaders({"metric", "value"});
    table.addRow({"global swaps", fmt::format("{}", res.globalSwaps)});
    table.addRow({"vertical swaps", fmt::format("{}", res.verticalSwaps)});
    table.addRow({"median moves", fmt::format("{}", res.medianMoves)});
    table.addRow({"reorder moves", fmt::format("{}", res.reorderMoves)});
    table.addRow({"cluster moves", fmt::format("{}", res.clusterMoves)});
    table.addRow({"HPWL before", fmt::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", fmt::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"overlapping pairs", fmt::format("{}", res.overlappingPairs)});
    table.addRow({"off row", fmt::format("{}", res.offRow)});
    table.addRow({"off site", fmt::format("{}", res.offSite)});
    table.addRow({"over fixed", fmt::format("{}", res.overFixed)});
    table.addRow({"seconds", fmt::format("{:.3f}", res.seconds)});
    table.emit();
}

}  // namespace

fastdpDM::fastdpDM(Design design) : d(std::move(design)) {}

fastdpSolution fastdpDM::run(const DetailPlaceParams &params) {
    ScopedTimer timer("detailplace");

    FastDetailedPlacer placer(d);
    const DetailPlaceResult result = placer.place(params);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return placer.solution();
}

}  // namespace ktplace::fastdp
