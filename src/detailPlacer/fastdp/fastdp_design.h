// @file fastdp_design.h
// The design as FastDP sees it.
//
// Deliberately not the shared Graph. FastDP cuts each row into spans and moves
// whole cells between them, so what it needs is the cells, the rows, and the
// netlist -- which it uses for the wirelength it trades against displacement, not
// for legality. The adaptor in src/adaptor builds one of these from the shared
// database; the placer never sees ktDM.

#pragma once

#include <array>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace ktplace::fastdp {

/// A cell to move. Its size, where it is, and whether it is pinned: everything
/// a detailed placer needs to decide whether it may move at all.
struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
};

/// A pin on a cell. Only its offset from the cell origin matters, for HPWL.
struct Pin {
    std::size_t cellId = 0;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// A net. FastDP minimises displacement, so it reads the netlist only to report
/// what the displacement cost in wirelength.
struct Net {
    std::vector<std::size_t> pins;
};

/// Half-perimeter wirelength over a placement. FastDP does not optimise this --
/// it minimises displacement -- but the wirelength that displacement costs is the
/// number that says whether the pass was worth running.
[[nodiscard]] double netlistHPWL(const std::vector<Cell> &cells, const std::vector<Net> &nets,
                                 const std::vector<Pin> &pins, const std::vector<double> &xs,
                                 const std::vector<double> &ys);

/// One placement region of a row. Coordinates are absolute and already resolved
/// from site units by the adaptor, so the placer never needs the pitch to read one.
struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
};

/// One placement row: its band and its site pitch.
struct RowInfo {
    double coordinate = 0.0;
    double height = 0.0;
    double sitePitch = 0.0;

    [[nodiscard]] double pitch() const noexcept {
        return sitePitch;
    }
    std::vector<Subrow> subrows;
};

/// The die bounding box, xlo ylo xhi yhi.
using DieBox = std::array<double, 4>;

/// One snapshot the placer wants drawn. The placer decides *when* a frame matters;
/// the host decides how to draw it, which keeps it free of plotting.
struct FrameInfo {
    std::string note;
    std::size_t step = 0;
    std::size_t numSteps = 0;
    double hpwl = 0.0;
    double hpwlInitial = 0.0;
    double overflow = 0.0;
    bool mandatory = false;
    std::string path;
};

/// Host hook: called with the position of every cell. Null disables drawing.
using FrameHook = std::function<void(const std::vector<float> &xs, const std::vector<float> &ys,
                                     const FrameInfo &info)>;

/// A whole design as FastDP sees it.
struct Design {
    std::vector<Cell> cells;
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::vector<RowInfo> rows;
    DieBox die{};
    /// Installed by the host. The placer never draws.
    FrameHook onFrame;
};

}  // namespace ktplace::fastdp
