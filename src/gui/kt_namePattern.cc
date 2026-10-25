#include "gui/kt_namePattern.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace::gui {

NamePattern::NamePattern(std::string_view pattern)
    : glob(pattern), literal(pattern.find_first_of("*?") == std::string_view::npos) {}

bool NamePattern::isMatch(std::string_view name) const {
    if (literal) {
        return name == glob;
    }
    std::size_t at = 0;
    std::size_t next = 0;
    std::size_t star = std::string::npos;
    std::size_t resume = 0;
    while (at < name.size()) {
        if (next < glob.size() && (glob[next] == '?' || glob[next] == name[at])) {
            ++at;
            ++next;
        } else if (next < glob.size() && glob[next] == '*') {
            star = next++;
            resume = at;
        } else if (star != std::string::npos) {
            next = star + 1;
            at = ++resume;
        } else {
            return false;
        }
    }
    while (next < glob.size() && glob[next] == '*') {
        ++next;
    }
    return next == glob.size();
}

bool NamePattern::isLiteral() const {
    return literal;
}

const std::string &NamePattern::getText() const {
    return glob;
}

std::vector<std::uint32_t> matchNames(const std::vector<std::string> &names,
                                      const NamePattern &pattern, std::size_t limit) {
    std::vector<std::uint32_t> found;
    for (std::uint32_t id = 0; id < names.size() && found.size() < limit; ++id) {
        if (pattern.isMatch(names[id])) {
            found.push_back(id);
        }
    }
    return found;
}

}  // namespace ktplace::gui
