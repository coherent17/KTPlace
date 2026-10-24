#pragma once

#include <cstdint>
#include <string>

namespace ktplace {

struct PlatformInfo {
    std::string host;
    std::string os;
    std::string cpu;
    std::string compiler;
    unsigned cores = 0;
    unsigned threads = 0;
    std::uint64_t memoryBytes = 0;
    std::uint64_t availableBytes = 0;

    [[nodiscard]] static PlatformInfo detect();
    void report() const;
};

}  // namespace ktplace
