// @file ktToMultiRowAdaptor.h
// Bridges the shared database and the multi-row legalizer.
//
// This legalizer never sees ktDM. It gets a multirow::multirowDM from here and
// answers with a multiRowSolution, which the flow commits through
// ktDM::setMultiRowSolution.

#pragma once

#include "datamodel/kt_dm.h"
#include "legalizer/multirow/multirow_dm.h"

#include <string>

namespace ktplace {

/// ktDM -> multirow::multirowDM.
///
/// Only what this pass reads: the cells and their sizes, the rows, the regions,
/// and just enough netlist to report the wirelength the pass cost. `plotDir` is
/// where the host should write frames when the pass asks for one; it never draws
/// itself, it hands the positions back through the frame hook installed here.
[[nodiscard]] multirow::multirowDM buildMultiRowDM(const ktDM &db, const std::string &plotDir);

}  // namespace ktplace
