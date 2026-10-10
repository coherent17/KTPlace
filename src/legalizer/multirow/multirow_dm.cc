#include "legalizer/multirow/multirow_dm.h"

#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

namespace ktplace::multirow {
namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside this pass can act on these, so they are
/// printed here and not returned.
void reportRun(const MultiRowLegalizeResult &res) {
    ktReportTable table("Multi-row legalization results");
    table.setHeaders({"metric", "value"});
    table.addRow({"movable cells", fmt::format("{}", res.movable)});
    table.addRow({"placed", fmt::format("{}", res.placed)});
    table.addRow({"unplaced", fmt::format("{}", res.unplaced)});
    table.addRow({"taller than a row", fmt::format("{}", res.multiRow)});
    table.addRow({"taller, placed", fmt::format("{}", res.multiRowPlaced)});
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

multirowDM::multirowDM(Design design) : d(std::move(design)) {}

multiRowSolution multirowDM::run(const MultiRowLegalizeParams &params) {
    ScopedTimer timer("legalize");

    MultiRowLegalizer legalizer(d);
    const MultiRowLegalizeResult result = legalizer.legalize(params);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return legalizer.solution();
}

}  // namespace ktplace::multirow
