#include "gui/kt_selectionSet.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

void sortUnique(std::vector<std::uint32_t> &ids) {
    std::ranges::sort(ids);
    const auto [first, last] = std::ranges::unique(ids);
    ids.erase(first, last);
}

}  // namespace

void SelectionSet::subscribe(Listener listener) {
    listeners.push_back(std::move(listener));
}

void SelectionSet::selectCells(std::vector<std::uint32_t> chosen) {
    focused = chosen.empty() ? std::nullopt : std::optional(chosen.front());
    selectedCells = std::move(chosen);
    sortUnique(selectedCells);
    notify();
}

void SelectionSet::addCells(std::span<const std::uint32_t> chosen) {
    if (chosen.empty()) {
        return;
    }
    selectedCells.insert(selectedCells.end(), chosen.begin(), chosen.end());
    sortUnique(selectedCells);
    focused = chosen.front();
    notify();
}

void SelectionSet::toggleCell(std::uint32_t cell) {
    const auto at = std::ranges::lower_bound(selectedCells, cell);
    if (at != selectedCells.end() && *at == cell) {
        selectedCells.erase(at);
        if (focused == cell) {
            focused = selectedCells.empty() ? std::nullopt : std::optional(selectedCells.front());
        }
    } else {
        selectedCells.insert(at, cell);
        focused = cell;
    }
    notify();
}

void SelectionSet::markNets(std::vector<std::uint32_t> chosen) {
    markedNets = std::move(chosen);
    sortUnique(markedNets);
    notify();
}

void SelectionSet::clear() {
    if (selectedCells.empty() && markedNets.empty()) {
        return;
    }
    selectedCells.clear();
    markedNets.clear();
    focused.reset();
    notify();
}

const std::vector<std::uint32_t> &SelectionSet::cells() const {
    return selectedCells;
}

const std::vector<std::uint32_t> &SelectionSet::nets() const {
    return markedNets;
}

std::optional<std::uint32_t> SelectionSet::focus() const {
    return focused;
}

bool SelectionSet::contains(std::uint32_t cell) const {
    return std::ranges::binary_search(selectedCells, cell);
}

bool SelectionSet::empty() const {
    return selectedCells.empty() && markedNets.empty();
}

void SelectionSet::notify() const {
    for (const Listener &listener : listeners) {
        listener();
    }
}

}  // namespace ktplace::gui
