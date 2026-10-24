#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace ktplace::gui {

class SelectionSet {
public:
    using Listener = std::function<void()>;

    void subscribe(Listener listener);

    void selectCells(std::vector<std::uint32_t> chosen);
    void addCells(std::span<const std::uint32_t> chosen);
    void toggleCell(std::uint32_t cell);
    void markNets(std::vector<std::uint32_t> chosen);
    void clear();

    [[nodiscard]] const std::vector<std::uint32_t> &cells() const;
    [[nodiscard]] const std::vector<std::uint32_t> &nets() const;
    [[nodiscard]] std::optional<std::uint32_t> focus() const;
    [[nodiscard]] bool contains(std::uint32_t cell) const;
    [[nodiscard]] bool empty() const;

private:
    void notify() const;

    std::vector<std::uint32_t> selectedCells;
    std::vector<std::uint32_t> markedNets;
    std::optional<std::uint32_t> focused;
    std::vector<Listener> listeners;
};

}  // namespace ktplace::gui
