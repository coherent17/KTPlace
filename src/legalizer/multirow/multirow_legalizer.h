// @file multirow_legalizer.h
// Legalizer that can place cells taller than one row

#pragma once

#include "datamodel/kt_solutionMgr.h"
#include "legalizer/multirow/multirow_design.h"
#include "legalizer/multirow/multirow_options.h"

#include <memory>

namespace ktplace {

class constraintMgr;

struct MultiRowLegalizeResult {
    std::size_t movable = 0;
    std::size_t placed = 0;
    std::size_t unplaced = 0;
    std::size_t multiRow = 0;
    std::size_t multiRowPlaced = 0;
    std::size_t macros = 0;
    std::size_t macrosPlaced = 0;
    double displacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

/// Places movable cells into the rows, including cells that span several of them.
///
/// A single-row legalizer cannot: a cell of height h > row height has no row to go
/// in, so it is left where global placement put it, overlapping. This one slices
/// the rows instead of accepting them as they are. Slicing is what makes the
/// multi-row case fall out of the same machinery as the single-row case rather
/// than needing a second placer: a cell is cut out of every row it spans, leaving
/// a left segment, a right segment, and the space underneath it. A later cell
/// that fits in the resulting well is placed like any other cell.
class MultiRowLegalizer {
public:
    explicit MultiRowLegalizer(const multirow::Design &design);
    ~MultiRowLegalizer();

    MultiRowLegalizer(const MultiRowLegalizer &) = delete;
    MultiRowLegalizer &operator=(const MultiRowLegalizer &) = delete;

    MultiRowLegalizer(MultiRowLegalizer &&) noexcept;
    MultiRowLegalizer &operator=(MultiRowLegalizer &&) noexcept;

    MultiRowLegalizeResult legalize(const MultiRowOptions &options = {});

    /// Where the last run put every cell. Nothing is written anywhere else: the
    /// caller commits this through ktDM.
    [[nodiscard]] multiRowSolution solution() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
