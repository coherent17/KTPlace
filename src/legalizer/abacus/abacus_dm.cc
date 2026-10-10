#include "legalizer/abacus/abacus_dm.h"

#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"

namespace ktplace::abacus {
namespace {

/// The pass's own summary, emitted from the run that produced the numbers rather
/// than by the caller. Nothing outside Abacus can act on these, so they are
/// printed here and not returned.
void reportRun(const LegalizeResult &res) {
    ktReportTable table("Abacus legalization results");
    table.setHeaders({"metric", "value"});
    table.addRow({"cells placed", fmt::format("{}", res.cellsPlaced)});
    table.addRow({"unplaced", fmt::format("{}", res.unplaced)});
    table.addRow({"HPWL before", fmt::format("{:.6e}", res.hpwlBefore)});
    table.addRow({"HPWL after", fmt::format("{:.6e}", res.hpwlAfter)});
    table.addRow({"total displacement", fmt::format("{:.6e}", res.totalSquaredDisplacement)});
    table.addRow({"max displacement", fmt::format("{:.6}", res.maxDisplacement)});
    table.addRow({"overlapping pairs", fmt::format("{}", res.overlappingPairs)});
    table.addRow({"outside rows", fmt::format("{}", res.outOfRows)});
    table.addRow({"over fixed", fmt::format("{}", res.overFixed)});
    table.addRow({"commit failures", fmt::format("{}", res.commitFailures)});
    table.addRow({"seconds", fmt::format("{:.3f}", res.seconds)});
    table.emit();
}

}  // namespace

abacusDM::abacusDM(Design design) : d(std::move(design)) {}

abacusSolution abacusDM::run(const LegalizeParams &params) {
    ScopedTimer timer("legalize");

    AbacusLegalizer legalizer(d);
    const LegalizeResult result = legalizer.legalize(params);
    reportRun(result);

    // Only the placement crosses the boundary. The statistics are printed above
    // and dropped here, so the flow only ever sees where the cells went.
    return legalizer.solution();
}

}  // namespace ktplace::abacus
