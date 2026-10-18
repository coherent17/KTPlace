#include "util/kt_history.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <utility>

namespace ktplace {

CommandHistory::CommandHistory(std::string file) : path(std::move(file)) {
    if (path.empty()) {
        return;
    }
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
    }
    if (lines.size() > capacity) {
        lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(capacity));
        std::ofstream out(path, std::ios::trunc);
        for (const std::string &kept : lines) {
            out << kept << '\n';
        }
    }
}

void CommandHistory::add(const std::string &command) {
    if (command.empty() || command.find('\n') != std::string::npos ||
        (!lines.empty() && lines.back() == command)) {
        return;
    }
    lines.push_back(command);
    if (lines.size() > capacity) {
        lines.erase(lines.begin());
    }
    if (!path.empty()) {
        std::ofstream(path, std::ios::app) << command << '\n';
    }
}

std::string CommandHistory::defaultFile() {
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        return {};
    }
    return (std::filesystem::path(home) / ".ktplace_history").string();
}

}  // namespace ktplace
