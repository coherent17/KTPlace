// @file ktToSimplAdaptor.h
// Bridges the shared database and the SimPL library.
//
// SimPL is a standalone library: it knows nothing about ktDM. It gets a
// simpl::simplDM from here, answers with a simplSolution, and that solution is
// committed back through ktDM::setGPSolution. This is the only place the two
// meet, in both directions.

#pragma once

#include "datamodel/kt_dm.h"
#include "datamodel/kt_solutionMgr.h"
#include "placer/simpl/simpl_dm.h"

#include <string>

namespace ktplace {

/// ktDM -> simpl::simplDM.
///
/// Builds a SimPL-owned database from the shared one and hands it over. The
/// result owns its design outright: SimPL holds no reference back to ktDM, so a
/// stale database cannot be mistaken for a current one.
///
/// `plotDir` is where the host should write frames when SimPL asks for one.
/// SimPL itself never draws; it hands the positions back through the frame hook
/// installed here, and the drawing happens with the shared netlist.
[[nodiscard]] simpl::simplDM buildSimplDM(const ktDM &db, const std::string &plotDir);

}  // namespace ktplace
