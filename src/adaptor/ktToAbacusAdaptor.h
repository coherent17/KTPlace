// @file ktToAbacusAdaptor.h
// Bridges the shared database and the Abacus legalizer.
//
// Abacus never sees ktDM. It gets an abacus::abacusDM from here and answers with
// an abacusSolution, which the flow commits through ktDM::setLegalizationSolution.

#pragma once

#include "datamodel/kt_dm.h"
#include "legalizer/abacus/abacus_dm.h"

#include <string>

namespace ktplace {

/// ktDM -> abacus::abacusDM.
///
/// Only what Abacus reads: the cells and their sizes, the rows, the regions, and
/// just enough netlist to report the wirelength legalization cost. `plotDir` is
/// where the host should write frames when Abacus asks for one; Abacus itself
/// never draws, it hands the positions back through the frame hook installed here.
[[nodiscard]] abacus::abacusDM buildAbacusDM(const ktDM &db, const std::string &plotDir);

}  // namespace ktplace
