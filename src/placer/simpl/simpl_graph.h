// @file simpl_graph.h
// SimPL's own netlist model.
//
// Deliberately not the shared Graph: SimPL is its own library and knows nothing
// about ktDM. The adaptor in ktToSimplAdaptor fills this in.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ktplace::simpl {

enum class PinRole {
    Driver,
    Receiver,
};

/// A placed object: a standard cell, a macro, or a fixed pad.
struct Cell {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool isFixed = false;
    bool isTerminal = false;
    /// Index into the design's fence regions, or kNoRegion.
    int regionId = -1;
};

/// A pin on a cell. Offsets are from the cell origin.
struct Pin {
    std::size_t cellId = 0;
    std::size_t netId = 0;
    PinRole role = PinRole::Receiver;
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// A net and the pins on it.
struct Net {
    double weight = 1.0;
    std::vector<std::size_t> pins;
};

/// The netlist as SimPL sees it: flat arrays with net-to-pin and pin-to-cell
/// links, which is the shape the quadratic solver wants to walk.
class Graph {
public:
    [[nodiscard]] std::size_t getNumCells() const noexcept {
        return cells.size();
    }
    [[nodiscard]] std::size_t getNumNets() const noexcept {
        return nets.size();
    }
    [[nodiscard]] std::size_t getNumPins() const noexcept {
        return pins.size();
    }

    [[nodiscard]] const Cell &getCell(std::size_t id) const {
        return cells.at(id);
    }
    [[nodiscard]] const Net &getNet(std::size_t id) const {
        return nets.at(id);
    }
    [[nodiscard]] const Pin &getPin(std::size_t id) const {
        return pins.at(id);
    }

    [[nodiscard]] const std::vector<std::size_t> &getNetPins(std::size_t netId) const {
        return getNet(netId).pins;
    }

    void addCell(const Cell &c) {
        cells.push_back(c);
    }
    void addNet(double weight) {
        nets.push_back(Net{weight, {}});
    }
    void addPin(const Pin &p, std::size_t netId) {
        pins.push_back(p);
        nets.at(netId).pins.push_back(pins.size() - 1);
    }

private:
    std::vector<Cell> cells;
    std::vector<Net> nets;
    std::vector<Pin> pins;
};

}  // namespace ktplace::simpl
