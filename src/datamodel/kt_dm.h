// @file kt_dm.h// Core data model: the design as parsed, and nothing more.


#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

// The design, as the reader parsed it: the netlist graph, the row structure, the
// die box and the placement regions the design declared. A placement stage reads
// this and derives whatever structures it needs; it does not add to it.

class ktDM {
public:
    // The destructor and the move operations used to be declared because the PIMPL
    // needed them: a unique_ptr cannot be default-constructed or trivially moved,
    // so each one had to be spelled out and defined out of line. None of them was
    // ever used -- a ktDM is only ever held behind a unique_ptr, which moves the
    // pointer and never the object. All implicit now, and header-only, so a reader
    // that includes this header does not have to link against them.
    // Spelled out because declaring a copy constructor -- even a deleted one --
    // suppresses the implicit default constructor, and a reader default-constructs
    // this to fill it in.
    ktDM() = default;

    ktDM(const ktDM &) = delete;
    ktDM &operator=(const ktDM &) = delete;

    // Placement regions ("fences") the design declared. They belong to the
    // design, so they live here rather than in whatever reader happened to
    // produce it: the reader fills them in, and every stage reads them from the
    // database instead of being handed a pointer. Empty when the format has none.
    [[nodiscard]] const constraintMgr &constraints() const;
    [[nodiscard]] bool hasFences() const;

    // One way this placement is wrong.
    struct Defect {
        std::string what;
        std::size_t count = 0;
    };

    // An independent pass over the placement as it will be written: overlaps,
    // cells outside the die, cells outside their fence. The placement stages each
    // self-check, but only for what they knew to ask about, and a stage reporting
    // zero overlaps while the delivered file has them is the failure worth
    // catching -- the file is what the next tool reads. Empty means it passed.
    [[nodiscard]] std::vector<Defect> verify() const;

    // Half-perimeter wirelength of the placement as it currently stands, pin to
    // pin over every net. This is the number the paper reports, so it is measured
    // once, here, rather than separately by each stage.
    [[nodiscard]] double hpwl() const;

    // Hands the design's fences over, for a reader that has just parsed them.
    void setConstraints(constraintMgr fences);

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
    [[nodiscard]] const std::vector<RowInfo> &getRows() const;

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
        return graph_;
    }
    [[nodiscard]] const Graph &getGraph() const {
        return graph_;
    }

    // Clear all data
    void clear();

    // Statistics
    [[nodiscard]] std::pair<std::size_t, std::size_t> getStats() const;

    // Movable demand against the rows, which decides whether the design fits.
    // Fixed cells already occupy the rows rather than compete for them, so their
    // area is reported but is not demand.
    struct Utilisation {
        double cellArea = 0.0;
        double fixedArea = 0.0;
        double rowArea = 0.0;
        double rowHeight = 0.0;
        std::size_t multiRow = 0;
        std::size_t cells = 0;
    };

    [[nodiscard]] Utilisation measureUtilisation() const;

    // How the design loaded: what it contains, and what placement it shipped with.
    void report() const;

    // Said before placement runs, not after legalization fails. A legalizer handed
    // a design that does not fit produces an illegal placement and a table of
    // confident numbers; checking the arithmetic first turns "the legalizer is
    // broken" into "this design is 102% full".
    void reportUtilisation() const;
    // (numCells, numNets)

private:
    // No PIMPL. It was there to keep Graph and constraintMgr out of this header,
    // and it never managed that: getGraph() returns a Graph& and constraints()
    // returns a constraintMgr&, so both had to be complete types at every call
    // site regardless -- and four component headers already include
    // kt_constraintMgr.h for exactly that reason. What it did buy is a private
    // copy of the row structure, RowData, that had to be converted into the
    // public RowInfo on every getRows() call. Both are gone: this header includes
    // the two types it hands out by reference, and stores RowInfo directly.
    Graph graph_;
    std::vector<RowInfo> rows_;
    double dieXMin_ = 0.0;
    double dieYMin_ = 0.0;
    double dieXMax_ = 0.0;
    double dieYMax_ = 0.0;
    constraintMgr fences_;
};

// The region a movable cell is allowed to occupy: {xMin, yMin, xMax, yMax}.// One definition, shared by the placer and by the legality check, because the two// disagreeing is how a legal placement gets reported as illegal. It is the union// of three statements about the region, each of which may be absent:// - the bounding box of the fixed cells, which in a Bookshelf design is the I/O// pad ring and is usually a good approximation of the die;// - the declared die area, when the format carries one and it contains every// fixed cell;// - the rows, which are the authoritative statement of where a cell may go.// The rows are unioned in rather than used only as a fallback, because for adaptec3// they reach past the fixed cells -- its rows start at y=58 while its fixed cells// start at y=82 -- so a box built from the fixed cells alone excludes the bottom// row, and every cell the legalizer correctly put in that row is then reported as// outside the die.

[[nodiscard]] std::array<double, 4> placementDieBox(const ktDM &db);

}  // namespace ktplace
