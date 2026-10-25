#include "util/kt_platform.h"

#include "util/kt_reportTable.h"

#include <array>
#include <cstdint>
#include <format>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <utility>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace ktplace {

namespace {

std::string getTrimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\"");
    if (first == std::string_view::npos) {
        return {};
    }
    return std::string(text.substr(first, text.find_last_not_of(" \t\"") - first + 1));
}

std::string getValueAfter(std::string_view line, char separator) {
    const auto at = line.find(separator);
    return at == std::string_view::npos ? std::string() : getTrimmed(line.substr(at + 1));
}

std::string getHostName() {
    std::array<char, 256> name{};
    return gethostname(name.data(), name.size() - 1) == 0 ? std::string(name.data()) : "";
}

std::string getKernel() {
    utsname uts{};
    if (uname(&uts) != 0) {
        return {};
    }
    return std::format("{} {} {}", uts.sysname, uts.release, uts.machine);
}

std::string getCompilerName() {
#if defined(__clang__)
    return std::format("clang {}", __clang_version__);
#elif defined(__GNUC__)
    return std::format("g++ {}", __VERSION__);
#else
    return "unknown";
#endif
}

std::uint64_t getPhysicalMemory() {
    const long pages = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGE_SIZE);
    return pages > 0 && pageSize > 0
               ? static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(pageSize)
               : 0;
}

#if defined(__APPLE__)

std::string getSysctlString(const char *name) {
    std::size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return {};
    }
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
        return {};
    }
    return std::string(value.c_str());
}

unsigned getSysctlUnsigned(const char *name) {
    int value = 0;
    std::size_t size = sizeof(value);
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? static_cast<unsigned>(value) : 0;
}

void detectSystem(PlatformInfo &info) {
    info.os = std::format("macOS {} ({})", getSysctlString("kern.osproductversion"), getKernel());
    info.cpu = getSysctlString("machdep.cpu.brand_string");
    info.cores = getSysctlUnsigned("hw.physicalcpu");
}

#else

std::string getOsRelease() {
    std::ifstream in("/etc/os-release");
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("PRETTY_NAME=")) {
            return getValueAfter(line, '=');
        }
    }
    return {};
}

std::uint64_t getAvailableMemory() {
    std::ifstream in("/proc/meminfo");
    for (std::string line; std::getline(in, line);) {
        if (line.starts_with("MemAvailable:")) {
            return std::stoull(getValueAfter(line, ':')) * 1024;
        }
    }
    return 0;
}

void detectSystem(PlatformInfo &info) {
    const std::string release = getOsRelease();
    info.os = release.empty() ? getKernel() : std::format("{} ({})", release, getKernel());
    info.availableBytes = getAvailableMemory();

    std::ifstream in("/proc/cpuinfo");
    std::set<std::pair<std::string, std::string> > physicalCores;
    std::string package;
    for (std::string line; std::getline(in, line);) {
        if (info.cpu.empty() && line.starts_with("model name")) {
            info.cpu = getValueAfter(line, ':');
        } else if (line.starts_with("physical id")) {
            package = getValueAfter(line, ':');
        } else if (line.starts_with("core id")) {
            physicalCores.emplace(package, getValueAfter(line, ':'));
        }
    }
    info.cores = static_cast<unsigned>(physicalCores.size());
}

#endif

std::string getGibibytes(std::uint64_t bytes) {
    return std::format("{:.1f} GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

}  // namespace

PlatformInfo PlatformInfo::detect() {
    PlatformInfo info;
    info.host = getHostName();
    info.compiler = getCompilerName();
    info.threads = std::thread::hardware_concurrency();
    info.memoryBytes = getPhysicalMemory();
    detectSystem(info);
    if (info.cpu.empty()) {
        info.cpu = "unknown";
    }
    if (info.cores == 0) {
        info.cores = info.threads;
    }
    return info;
}

void PlatformInfo::report() const {
    ktReportTable table("Platform");
    table.add("host", host);
    table.add("os", os);
    table.add("cpu", cpu);
    table.add("cores / threads", std::format("{} / {}", cores, threads));
    table.add("memory", availableBytes > 0
                            ? std::format("{} ({} available)", getGibibytes(memoryBytes),
                                          getGibibytes(availableBytes))
                            : getGibibytes(memoryBytes));
    table.add("compiler", compiler);
    table.emit();
}

}  // namespace ktplace
