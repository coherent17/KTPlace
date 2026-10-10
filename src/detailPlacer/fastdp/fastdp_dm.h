// @file fastdp_dm.h
// FastDP's own database.
//
// It does not read ktDM and does not write to it. It runs against a
// fastdp::Design -- cells, rows, and the netlist it reports wirelength over --
// and hands its answer back from here.

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_design.h"
#include "detailPlacer/fastdp/fastdp_placer.h"

#include <string>

namespace ktplace::fastdp {

class fastdpDM {
public:
    /// Takes ownership of the design the adaptor built.
    explicit fastdpDM(Design design);

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
    fastdpSolution run(const DetailPlaceParams &params);

private:
    Design d;
};

}  // namespace ktplace::fastdp
