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

    [[nodiscard]] double getParallelism() const {
        return wallSeconds > 0.0 ? cpuSeconds / wallSeconds : 0.0;
    }
};

class TimerRegistry {
public:
    static TimerRegistry &getInstance();

    TimerRegistry(const TimerRegistry &) = delete;
    TimerRegistry &operator=(const TimerRegistry &) = delete;

    void record(std::string name, double wallSeconds, double cpuSeconds);

    /// Called when a timer starts and stops, so the report can list timers in the
    /// order they first ran, each indented under the timer it ran inside.
    void started(const std::string &name);
    void stopped();

    [[nodiscard]] const TimerStats *find(const std::string &name) const;

    [[nodiscard]] std::vector<std::pair<std::string, TimerStats>> getSnapshot() const;

    void reset();

    void report() const;

private:
    TimerRegistry() = default;

    mutable std::mutex mutex;
    std::map<std::string, TimerStats> stats;
    struct FirstRun {
        std::size_t order = 0;
        std::size_t depth = 0;
    };
    std::map<std::string, FirstRun> firstRun;
    std::size_t depth = 0;
};

class Stopwatch {
public:
    [[nodiscard]] double getElapsedSeconds() const;

    [[nodiscard]] double getCpuElapsedSeconds() const;

private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::clock_t cpuStart = std::clock();
};

class ScopedTimer {
public:
    explicit ScopedTimer(std::string name, bool announceOnExit = false);

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

    ~ScopedTimer();

    [[nodiscard]] double getElapsedSeconds() const;

    [[nodiscard]] double getCpuElapsedSeconds() const;

private:
    std::string timerName;
    Stopwatch stopwatch;
    bool announce = false;
};

/// The largest resident set this process has had, in bytes.
[[nodiscard]] std::size_t getPeakResidentBytes();

}  // namespace ktplace
