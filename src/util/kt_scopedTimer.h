#pragma once

#include <chrono>
#include <cstddef>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

struct TimerStats {
    double wallSeconds = 0.0;
    double cpuSeconds = 0.0;
    std::size_t calls = 0;

    [[nodiscard]] double parallelism() const {
        return wallSeconds > 0.0 ? cpuSeconds / wallSeconds : 0.0;
    }
};

class TimerRegistry {
public:
    static TimerRegistry &instance();

    TimerRegistry(const TimerRegistry &) = delete;
    TimerRegistry &operator=(const TimerRegistry &) = delete;

    void record(std::string name, double wallSeconds, double cpuSeconds);

    [[nodiscard]] bool isEnabled(const std::string &name) const;

    [[nodiscard]] const TimerStats *find(const std::string &name) const;

    [[nodiscard]] std::vector<std::pair<std::string, TimerStats>> snapshot() const;

    [[nodiscard]] double totalWallSeconds() const;

    [[nodiscard]] double totalCpuSeconds() const;

    void reset();

    void report() const;

private:
    TimerRegistry() = default;

    mutable std::mutex mutex;
    std::map<std::string, TimerStats> stats;
    std::map<std::string, bool> enabled;
};

class ScopedTimer {
public:
    explicit ScopedTimer(std::string name);

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

    ~ScopedTimer();

    void lap();

    [[nodiscard]] double elapsedSeconds() const;

    [[nodiscard]] double cpuElapsedSeconds() const;

    [[nodiscard]] const std::string &name() const;

private:
    std::string timerName;
    std::chrono::steady_clock::time_point start;
    std::clock_t cpuStart = 0;
};

}  // namespace ktplace