// @file kt_dm.h
// The design as the reader parsed it.

#pragma once

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_die.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_rowFence.h"
#include "datamodel/kt_solutionMgr.h"
#include "geometry/kt_geometry.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {


class ktDM {
public:
    ktDM() = default;

    ktDM(const ktDM &) = delete;
    ktDM &operator=(const ktDM &) = delete;

    [[nodiscard]] const constraintMgr &getConstraints() const;
    [[nodiscard]] bool hasFences() const;

    // One way this placement is wrong.
    struct Defect {
        std::string what;
        std::size_t count = 0;
    };

    [[nodiscard]] std::vector<Defect> getDefects() const;

    // Checks the placement as it now stands and reports it. Called where a stage
    // commits its solution, so whoever hands a placement on is the one that says
    // whether it is legal, and each stage's own self-check has something
    // independent to disagree with.
    void checkPlacement() const;

    [[nodiscard]] double getHpwl() const;

    void setConstraints(constraintMgr fences);

    void setSimplSolution(const simplSolution &solution);
    void setNtuplace1Solution(const ntuplace1Solution &solution);

    /// The same, for a legalization stage. Kept as its own name so a reader can
    /// tell which stage produced the placement.
    void setAbacusSolution(const abacusSolution &solution);
    void setMultiRowSolution(const multiRowSolution &solution);
    void setFastdpSolution(const fastdpSolution &solution);


    // Cell management
    void markIoPin(const std::string &name);
    [[nodiscard]] std::size_t addCell(const std::string &name, double width, double height,
                                      bool isTerminal = false);
    [[nodiscard]] bool hasCell(const std::string &name) const;
    [[nodiscard]] std::size_t getCellId(const std::string &name) const;
    [[nodiscard]] std::size_t getNumCells() const;
    [[nodiscard]] std::size_t getNumTerminals() const;

    // Net management
    [[nodiscard]] std::size_t addNet(const std::string &name, double weight = 1.0);
    [[nodiscard]] bool hasNet(const std::string &name) const;
    [[nodiscard]] std::size_t getNetId(const std::string &name) const;
    [[nodiscard]] std::size_t getNumNets() const;

    // Pin management
    [[nodiscard]] std::size_t addPin(const std::string &cellName, const std::string &netName,
                                     const Point &offset, bool isInput);
    [[nodiscard]] std::size_t getNumPins() const;

    // Placement coordinates
    void setCellPosition(std::size_t cellId, const Point &position);
    void setCellPosition(const std::string &cellName, const Point &position);
    [[nodiscard]] Point getCellPosition(std::size_t cellId) const;
    [[nodiscard]] Point getCellPosition(const std::string &cellName) const;

    // Cell fixed/movable status
    void setCellFixed(std::size_t cellId, bool fixed);
    void setCellFixed(const std::string &cellName, bool fixed);

    [[nodiscard]] std::size_t addRow(double coordinate, double height, double sitewidth,
                                     double sitespacing);
    [[nodiscard]] std::size_t addSubrow(std::size_t rowId, double originX, double numSites);
    [[nodiscard]] std::size_t getNumRows() const;
    [[nodiscard]] const std::vector<RowInfo> &getRows() const;

    [[nodiscard]] const DieInfo &getDie() const {
        return dieInfo;
    }

    void setDieArea(const BBox &box);

    [[nodiscard]] BBox getPlacementDieBox() const;

    // Movable cells too tall for any single row, against the shortest row that has
    // a subrow. These are what a single-row legalizer cannot place, so the count is
    // what says whether the design needs a multi-height legalizer at all.
    [[nodiscard]] std::size_t getMultiRowCells() const;
    [[nodiscard]] double getMacroHeight() const;
    [[nodiscard]] std::vector<std::vector<RowPiece>> getFencedSubrows() const;


    /// Pin ids are what these return, not vertex ids.

    [[nodiscard]] Graph &getGraph() {
        return graph;
    }
    [[nodiscard]] const Graph &getGraph() const {
        return graph;
    }

    // Clear all data
    void clear();

    struct Utilisation {
        double cellArea = 0.0;
        double fixedArea = 0.0;
        double rowArea = 0.0;
        double rowHeight = 0.0;
        std::size_t multiRow = 0;
        std::size_t cells = 0;
    };

    void report() const;
    void reportStatistics() const;
    void reportUtilisation() const;

    /// 32-bit FNV-1a over every cell's position, size and fixed flag and every pin's
    /// cell, net and offset: equal checksums mean the same design and placement.
    [[nodiscard]] std::uint32_t getChecksum() const;

private:
    [[nodiscard]] std::pair<std::size_t, std::size_t> getStats() const;
    [[nodiscard]] Utilisation measureUtilisation() const;


    Graph graph;
    DieInfo dieInfo;
    constraintMgr fences;
};

}  // namespace ktplace
