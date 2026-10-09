// @file kt_flowMgr.h// Flow manager to orchestrate the overall placement flow


#pragma once

#include "datamodel/kt_dm.h"

#include <memory>
#include <string>

namespace ktplace {

// Forward declarations
class BookshelfInputAdapter;

// Taken by reference in run() and never dereferenced here, so the flow layer
// stays independent of how a run is configured: a caller driving FlowMgr
// directly passes paths, and only the one that parses a command line has to
// know what kt_option is.
class kt_option;

// Flow manager to coordinate the overall placement flow// Manages the complete flow from input loading through placement execution// to output generation. Coordinates between adapters, PlacementDB, and// placement algorithms.

class FlowMgr {
public:
    /// Constructor
    FlowMgr();

    /// Destructor
    ~FlowMgr();

    // Copy semantics (deleted)
    FlowMgr(const FlowMgr &) = delete;
    FlowMgr &operator=(const FlowMgr &) = delete;

    // Move semantics
    FlowMgr(FlowMgr &&) noexcept;
    FlowMgr &operator=(FlowMgr &&) noexcept;

    // Run the complete placement flow// Executes the full placement flow: load input, run placement algorithm, write output.// All other operations are handled internally via the PIMPL implementation.// visualization snapshots (see SimplePlacer::place).

    void run(const kt_option &options);

    void run(const std::string &inputDirPath, const std::string &outputPath,
             const std::string &algorithm = "simpl", const std::string &plotDir = "");

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
