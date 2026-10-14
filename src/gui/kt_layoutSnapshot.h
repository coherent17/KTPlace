// @file kt_layoutSnapshot.h
// An immutable copy of what the layout view draws: the die, the rows and every
// cell's rectangle, plus a spatial index for picking.
//
// The design lives on the engine thread and is changed by the stages, so the
// GUI never reads it directly. After each script the engine captures one of
// these and hands it over as a shared_ptr<const>; the view can then draw and
// hit-test it at leisure while the engine goes on to the next command.
//
// Capture is a linear copy, a few milliseconds per hundred thousand cells, and
// it runs on the engine thread so the window is not held up by it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace ktplace {

class ktSession;

struct LayoutSnapshot {
    /// Fixed covers macros, including Bookshelf terminals the size of one;
    /// Terminal is an I/O pad.
    enum class Kind : std::uint8_t { Movable, Fixed, Terminal };

    /// One cell: the lower-left corner and the size, in design units.
    struct Cell {
        float x = 0.0F;
        float y = 0.0F;
        float w = 0.0F;
        float h = 0.0F;
        Kind kind = Kind::Movable;
    };

    struct Row {
        float y = 0.0F;  ///< bottom edge
        float h = 0.0F;
        float xlo = 0.0F;
        float xhi = 0.0F;
    };

    std::vector<Cell> cells;
    std::vector<Row> rows;
    /// The die as {xlo, ylo, xhi, yhi}.
    std::array<double, 4> die{};
    double hpwl = 0.0;
    /// Median movable cell width and height, which decide how the view draws.
    float typicalWidth = 1.0F;
    float typicalHeight = 1.0F;

    /// Copy the session's current placement. Engine thread only; the session
    /// must hold a design.
    [[nodiscard]] static std::shared_ptr<const LayoutSnapshot> capture(const ktSession &session);
    /// The same, with every cell at the lower-left corner given by @p x and
    /// @p y (indexed by cell id) instead of where the design has it, and @p hpwl
    /// as the wirelength: a stage's placement in the middle of the stage, before
    /// it is written back. Falls back to the design's positions when the arrays
    /// are not one entry per cell.
    [[nodiscard]] static std::shared_ptr<const LayoutSnapshot> capture(const ktSession &session,
                                                                       const std::vector<float> &x,
                                                                       const std::vector<float> &y,
                                                                       double hpwl);

    /// The cell under the point, preferring the smallest one when cells overlap
    /// (a standard cell drawn over a macro). nullopt where there is none.
    [[nodiscard]] std::optional<std::size_t> cellAt(double x, double y) const;

    /// Build the picking index. capture() calls it; a snapshot assembled by
    /// hand (in a test) has to call it before cellAt().
    void buildIndex();

private:
    // A uniform grid over the die. Each bin lists the cells that overlap it, as
    // one flat array addressed through binStart (compressed rows), so a pick is
    // one bin's worth of rectangle tests.
    int gridX_ = 0;
    int gridY_ = 0;
    double binW_ = 1.0;
    double binH_ = 1.0;
    std::vector<std::uint32_t> binStart_;
    std::vector<std::uint32_t> binCells_;

    [[nodiscard]] int binOf(double v, double lo, double size, int count) const;
};

}  // namespace ktplace
