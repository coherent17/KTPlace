#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ktplace {

template <typename T>
class PackedLists {
public:
    [[nodiscard]] std::size_t size() const noexcept {
        return firstItem.size() - 1;
    }
    [[nodiscard]] std::span<const T> operator[](std::size_t list) const {
        return std::span(items).subspan(firstItem[list], firstItem[list + 1] - firstItem[list]);
    }
    void add(std::span<const T> list) {
        items.insert(items.end(), list.begin(), list.end());
        firstItem.push_back(items.size());
    }

private:
    std::vector<std::size_t> firstItem{0};
    std::vector<T> items;
};

}  // namespace ktplace
