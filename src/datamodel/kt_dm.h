/**
 * @file kt_dm.h
 * @brief Core data model for placement database (PIMPL pattern)
 */

#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

namespace ktplace {

// Forward declarations
class Cell;
class Net;
class Pin;
class Row;
class Graph;

/**
 * @brief Main placement database class using PIMPL pattern
 * 
 * Stores all placement-related information including cells, nets, pins,
 * and placement constraints. Uses PIMPL to minimize compilation dependencies.
 */
class PlacementDB {
public:
    /// Default constructor
    PlacementDB();

    /// Destructor (defaulted, PIMPL handles cleanup)
    ~PlacementDB();

    // Copy semantics (deleted for now, can be added if needed)
    PlacementDB(const PlacementDB &) = delete;
    PlacementDB &operator=(const PlacementDB &) = delete;

    // Move semantics
    PlacementDB(PlacementDB &&) noexcept;
    PlacementDB &operator=(PlacementDB &&) noexcept;

    // Cell management
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
                                     double offsetX, double offsetY, bool isInput);
    [[nodiscard]] std::size_t getNumPins() const;

    // Placement coordinates
    void setCellPosition(std::size_t cellId, double x, double y);
    void setCellPosition(const std::string &cellName, double x, double y);
    [[nodiscard]] std::pair<double, double> getCellPosition(std::size_t cellId) const;
    [[nodiscard]] std::pair<double, double> getCellPosition(const std::string &cellName) const;

    // Cell fixed/movable status
    void setCellFixed(std::size_t cellId, bool fixed);
    void setCellFixed(const std::string &cellName, bool fixed);
    [[nodiscard]] bool isCellFixed(std::size_t cellId) const;
    [[nodiscard]] bool isCellFixed(const std::string &cellName) const;

    // Row management
    [[nodiscard]] std::size_t addRow(double coordinate, double height, double sitewidth,
                                     double sitespacing, double numSites);
    [[nodiscard]] std::size_t getNumRows() const;

    // Bounding box
    void setDieArea(double xMin, double yMin, double xMax, double yMax);
    [[nodiscard]] std::pair<std::pair<double, double>, std::pair<double, double>> getDieArea()
        const;

    // Net access
    [[nodiscard]] const std::vector<std::size_t> &getNetPins(std::size_t netId) const;
    [[nodiscard]] const std::vector<std::size_t> &getCellPins(std::size_t cellId) const;

    // Access to the underlying netlist graph (read-only)
    [[nodiscard]] const Graph &getGraph() const {
        return getGraphImpl();
    }

    // Clear all data
    void clear();

    // Statistics
    [[nodiscard]] std::pair<std::size_t, std::size_t> getStats() const;  // (numCells, numNets)

private:
    const Graph &getGraphImpl() const;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
