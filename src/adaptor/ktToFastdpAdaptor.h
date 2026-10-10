// @file ktToFastdpAdaptor.h
// Bridges the shared database and FastDP.
//
// FastDP never sees ktDM. It gets a fastdp::fastdpDM from here and answers with a
// fastdpSolution, which the flow commits through ktDM::setDetailedPlaceSolution.

#pragma once

#include "datamodel/kt_dm.h"
#include "detailPlacer/fastdp/fastdp_dm.h"

#include <string>

namespace ktplace {

/// ktDM -> fastdp::fastdpDM.
///
/// Only what FastDP reads: the cells, the rows, and the netlist it reports the
/// wirelength cost of displacement over. `plotDir` is where the host should write
/// frames when the placer asks for one; it never draws itself, it hands the
/// positions back through the frame hook installed here.
[[nodiscard]] fastdp::fastdpDM buildFastdpDM(const ktDM &db, const std::string &plotDir);

}  // namespace ktplace
