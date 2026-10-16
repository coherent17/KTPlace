// @file fastdp_placer.h
// Detailed placement after Pan, Viswanathan and Chu, "An Efficient and Effective
// Detailed Placement Algorithm", ICCAD 2005: single-segment clustering, then
// global swap, vertical swap and local re-ordering until the wirelength stops
// improving, then single-segment clustering again.

#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_solutionMgr.h"
#include "detailPlacer/fastdp/fastdp_design.h"
#include "detailPlacer/fastdp/fastdp_options.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

struct DetailPlaceResult {
    std::size_t globalSwaps = 0;
    std::size_t verticalSwaps = 0;
    std::size_t reorderMoves = 0;
    std::size_t clusterMoves = 0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    /// Legality self-check, so a detailed placement bug shows up as a number.
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    double seconds = 0.0;
    double globalSeconds = 0.0;
    double verticalSeconds = 0.0;
    double reorderSeconds = 0.0;
    double clusterSeconds = 0.0;
};

class FastDetailedPlacer {
public:
    explicit FastDetailedPlacer(const fastdp::Design &design);
    ~FastDetailedPlacer();

    FastDetailedPlacer(const FastDetailedPlacer &) = delete;
    FastDetailedPlacer &operator=(const FastDetailedPlacer &) = delete;

    /// Improve the placement in place, writing positions back to the database.
    DetailPlaceResult place(const FastdpOptions &options = {});

    /// Where the last run put every cell. Nothing is written anywhere else: the
    /// caller commits this through ktDM.
    [[nodiscard]] fastdpSolution solution() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
