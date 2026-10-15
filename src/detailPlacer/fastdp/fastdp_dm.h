// @file fastdp_dm.h
// FastDP's own database.
//
// It does not read ktDM and does not write to it. It runs against a
// fastdp::Design -- cells, rows, and the netlist it reports wirelength over --
// and hands its answer back from here.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_design.h"
#include "detailPlacer/fastdp/fastdp_options.h"
#include "detailPlacer/fastdp/fastdp_placer.h"

#include <string>

namespace ktplace {

class ktDM;

namespace fastdp {

class fastdpDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit fastdpDM(Design design);

    [[nodiscard]] const FastdpOptions &options() const noexcept {
        return opts;
    }

    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }
    [[nodiscard]] Design &design() noexcept {
        return d;
    }

    /// Install the host's frame hook. The placer never draws on its own.
    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }

    /// Move the cells, and return where every cell ended up. The pass's
    /// statistics are reported from inside the run; they are not returned, because
    /// nothing outside FastDP can act on them.
    fastdpSolution run();

    FastdpOptions opts;

private:
    Design d;
};

/// ktDM -> fastdp::fastdpDM.
///
/// Only what this stage reads: the cells, the rows, and the netlist it reports the wirelength cost of
/// displacement over. The stage itself never
/// draws: it hands the positions to the visualizer, which decides where they go.
[[nodiscard]] fastdpDM buildFastdpDM(const ktDM &db);

}  // namespace fastdp
}  // namespace ktplace
