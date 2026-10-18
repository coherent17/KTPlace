#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace ktplace {

class CommandHistory {
public:
    static constexpr std::size_t capacity = 1000;

    explicit CommandHistory(std::string file = defaultFile());

    void add(const std::string &command);
    [[nodiscard]] const std::vector<std::string> &entries() const noexcept {
        return lines;
    }
    [[nodiscard]] const std::string &file() const noexcept {
        return path;
    }

    [[nodiscard]] static std::string defaultFile();

private:
    std::string path;
    std::vector<std::string> lines;
};

}  // namespace ktplace
