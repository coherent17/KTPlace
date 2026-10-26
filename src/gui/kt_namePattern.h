#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace::gui {

class NamePattern {
public:
    explicit NamePattern(std::string_view glob);

    [[nodiscard]] bool isMatch(std::string_view name) const;
    [[nodiscard]] bool isLiteral() const;

private:
    std::string glob;
    bool literal;
};

[[nodiscard]] std::vector<std::uint32_t> matchNames(
    const std::vector<std::string> &names, const NamePattern &pattern,
    std::size_t limit = std::numeric_limits<std::size_t>::max());

}  // namespace ktplace::gui
