#include "util/kt_scopedTimer.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <format>
#include <map>
#include <sys/resource.h>
#include <utility>

namespace ktplace {

TimerRegistry &TimerRegistry::instance() {
    static TimerRegistry registry;
    return registry;
}

void TimerRegistry::record(std::string name, double wallSeconds, double cpuSeconds) {
    if (wallSeconds < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = enabled.find(name);
    if (it != enabled.end() && !it->second) {
        return;
    }
    TimerStats &entry = stats[name];
    entry.wallSeconds += wallSeconds;
    entry.cpuSeconds += cpuSeconds > 0.0 ? cpuSeconds : 0.0;
    ++entry.calls;
}

void TimerRegistry::started(const std::string &name) {
    std::lock_guard<std::mutex> lock(mutex);
    firstRun.try_emplace(name, FirstRun{firstRun.size(), depth});
    ++depth;
}

void TimerRegistry::stopped() {
    std::lock_guard<std::mutex> lock(mutex);
    depth -= depth > 0 ? 1 : 0;
}

bool TimerRegistry::isEnabled(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = enabled.find(name);
    return it == enabled.end() || it->second;
}

const TimerStats *TimerRegistry::find(const std::string &name) const {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = stats.find(name);
    return it == stats.end() ? nullptr : &it->second;
}

std::vector<std::pair<std::string, TimerStats>> TimerRegistry::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::pair<std::string, TimerStats>> out;
    out.reserve(stats.size());
    for (const auto &[name, entry] : stats) {
        out.emplace_back(name, entry);
    }
    return out;
}

double TimerRegistry::totalWallSeconds() const {
    std::lock_guard<std::mutex> lock(mutex);
    double sum = 0.0;
    for (const auto &[name, entry] : stats) {
        sum += entry.wallSeconds;
    }
    return sum;
}

double TimerRegistry::totalCpuSeconds() const {
    std::lock_guard<std::mutex> lock(mutex);
    double sum = 0.0;
    for (const auto &[name, entry] : stats) {
        sum += entry.cpuSeconds;
    }
    return sum;
}

void TimerRegistry::reset() {
    std::lock_guard<std::mutex> lock(mutex);
    stats.clear();
    firstRun.clear();
    depth = 0;
}

void TimerRegistry::report() const {
    const auto all = snapshot();
    if (all.empty()) {
        return;
    }
    std::vector<std::pair<std::size_t, std::string>> ordered;
    std::map<std::string, std::size_t> indent;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto &[name, entry] : all) {
            const auto it = firstRun.find(name);
            const bool known = it != firstRun.end();
            ordered.emplace_back(known ? it->second.order : firstRun.size(), name);
            indent[name] = known ? it->second.depth : 0;
        }
    }
    std::ranges::stable_sort(ordered);
    const std::map<std::string, TimerStats> byName(all.begin(), all.end());

    ktReportTable table("Timings, in run order; a nested phase is part of the one above it");
    table.setHeaders({"phase", "wall", "cpu", "calls", "cpu/wall"});
    for (const auto &[order, name] : ordered) {
        const TimerStats &entry = byName.at(name);
        table.addRow({std::string(2 * indent[name], ' ') + name,
                      std::format("{:.3f}s", entry.wallSeconds),
                      std::format("{:.3f}s", entry.cpuSeconds), std::to_string(entry.calls),
                      std::format("{:.2f}x", entry.parallelism())});
    }
    table.emit();
}

ScopedTimer::ScopedTimer(std::string name, bool announceOnExit)
    : timerName(std::move(name)), announce(announceOnExit) {
    TimerRegistry::instance().started(timerName);
}

ScopedTimer::~ScopedTimer() {
    const double wall = elapsedSeconds();
    const double cpu = cpuElapsedSeconds();
    TimerRegistry::instance().stopped();
    TimerRegistry::instance().record(timerName, wall, cpu);
    if (announce) {
        ktlog.echo("finished in {:.3f}s wall, {:.3f}s cpu", wall, cpu);
    }
}

std::size_t peakResidentBytes() {
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024;
#endif
}

void ScopedTimer::lap() {
    const double wall = elapsedSeconds();
    const double cpu = cpuElapsedSeconds();
    stopwatch.restart();
    TimerRegistry::instance().record(timerName, wall, cpu);
}

void Stopwatch::restart() {
    start = std::chrono::steady_clock::now();
    cpuStart = std::clock();
}

double Stopwatch::elapsedSeconds() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now() - start).count();
}

double Stopwatch::cpuElapsedSeconds() const {
    const std::clock_t now = std::clock();
    if (cpuStart == 0 || now == static_cast<std::clock_t>(-1)) {
        return 0.0;
    }
    return static_cast<double>(now - cpuStart) / static_cast<double>(CLOCKS_PER_SEC);
}

double ScopedTimer::elapsedSeconds() const {
    return stopwatch.elapsedSeconds();
}

double ScopedTimer::cpuElapsedSeconds() const {
    return stopwatch.cpuElapsedSeconds();
}

const std::string &ScopedTimer::name() const {
    return timerName;
}

}  // namespace ktplace
