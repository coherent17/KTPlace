#include "kt_batch.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

namespace ktplace {

int runBatch(const kt_option &options) {
    const Stopwatch stopwatch;
    FlowMgr().run(options);
    ktlog.echo(
        "Placement completed successfully! {:.3f}s wall, {:.3f}s cpu, peak memory {:.1f} MiB",
        stopwatch.elapsedSeconds(), stopwatch.cpuElapsedSeconds(),
        static_cast<double>(peakResidentBytes()) / (1024.0 * 1024.0));
    return 0;
}

}  // namespace ktplace
