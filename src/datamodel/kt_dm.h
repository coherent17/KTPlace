/**
 * @file kt_dm.h
 * @brief Core data model for placement database (PIMPL pattern)
 */

#pragma once

#include <algorithm>
#include <array>
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
                                     double sitespacing);
    /// Append a subrow to a row. A row with no subrow has no placeable sites, so
    /// addRow alone is not enough to describe a placeable row.
    [[nodiscard]] std::size_t addSubrow(std::size_t rowId, double originX, double numSites);
    [[nodiscard]] std::size_t getNumRows() const;

    /// Row geometry as parsed from the Bookshelf .scl. Legalization and detailed
    /// placement both need the row pitch and the site width; the row list was
    /// previously write-only, so nothing downstream could align a cell to a row.
    /// One contiguous run of sites within a row. A row is split into subrows by
    /// whatever blocks it (macros, and in some formats extra SubrowOrigin
    /// lines), so a row is not one span of x and placing into it means choosing
    /// a subrow first.
    struct SubrowInfo {
        double originX = 0.0;
        double numSites = 0.0;

        [[nodiscard]] double xlo() const {
            return originX;
        }
        [[nodiscard]] double xhi(double spacing) const {
            return originX + numSites * spacing;
        }
    };

    struct RowInfo {
        double coordinate = 0.0;  ///< y of the row's bottom edge
        double height = 0.0;
        double sitewidth = 0.0;
        double sitespacing = 0.0;
        /// Site pitch; the subrow's sites sit at originX + k * sitePitch.
        [[nodiscard]] double pitch() const {
            return (sitespacing > 0.0) ? sitespacing : sitewidth;
        }
        /// One entry per SubrowOrigin/NumSites pair in the .scl, in file order.
        std::vector<SubrowInfo> subrows;

        [[nodiscard]] double xlo() const {
            return subrows.empty() ? 0.0 : subrows.front().xlo();
        }
        [[nodiscard]] double xhi() const {
            double hi = 0.0;
            for (const SubrowInfo &sr : subrows) {
                hi = std::max(hi, sr.xhi(pitch()));
            }
            return hi;
        }
    };
    [[nodiscard]] std::vector<RowInfo> getRows() const;

    // Bounding box
    void setDieArea(double xMin, double yMin, double xMax, double yMax);
    [[nodiscard]] std::pair<std::pair<double, double>, std::pair<double, double>> getDieArea()
        const;

    // Net access
    [[nodiscard]] const std::vector<std::size_t> &getNetPins(std::size_t netId) const;
    [[nodiscard]] const std::vector<std::size_t> &getCellPins(std::size_t cellId) const;

    // Access to the underlying netlist graph. The mutable overload exists for
    // the readers, which annotate cells while building the database (for
    // example stamping a placement region onto each cell).
    [[nodiscard]] Graph &getGraph() {
        return getGraphImpl();
    }
    [[nodiscard]] const Graph &getGraph() const {
        return getGraphImpl();
    }

    // Clear all data
    void clear();

    // Statistics
    [[nodiscard]] std::pair<std::size_t, std::size_t> getStats() const;  // (numCells, numNets)

private:
    Graph &getGraphImpl();
    const Graph &getGraphImpl() const;

    class Impl;
    std::unique_ptr<Impl> pImpl;
};

/**
 * @brief The region a movable cell is allowed to occupy: {xMin, yMin, xMax, yMax}.
 *
 * One definition, shared by the placer and by the legality check, because the two
 * disagreeing is how a legal placement gets reported as illegal. It is the union
 * of three statements about the region, each of which may be absent:
 *
 *  - the bounding box of the fixed cells, which in a Bookshelf design is the I/O
 *    pad ring and is usually a good approximation of the die;
 *  - the declared die area, when the format carries one and it contains every
 *    fixed cell;
 *  - the rows, which are the authoritative statement of where a cell may go.
 *
 * The rows are unioned in rather than used only as a fallback, because for adaptec3
 * they reach past the fixed cells -- its rows start at y=58 while its fixed cells
 * start at y=82 -- so a box built from the fixed cells alone excludes the bottom
 * row, and every cell the legalizer correctly put in that row is then reported as
 * outside the die.
 */
[[nodiscard]] std::array<double, 4> placementDieBox(const PlacementDB &db);

}  // namespace ktplace
