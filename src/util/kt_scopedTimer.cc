/**
 * @file kt_scopedTimer.cc
 * @brief Implementation of the KTPlace elapsed-time measurement
 */

#include "util/kt_scopedTimer.h"
#include "util/kt_reportTable.h"
#include "util/kt_log.h"
#include <ctime>

namespace ktplace {

double monotonicSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

namespace {
/// Processor seconds consumed by this process so far, over all threads.
}  // namespace

TimerRegistry &TimerRegistry::instance() {
    static TimerRegistry registry;
    return registry;
}

void TimerRegistry::record(std::string name, double wallSeconds, double cpuSeconds) {
    if (wallSeconds < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto it = enabled.find(name);
    if (it != enabled.end() && !it->second) {
        return;  // measuring continues, recording is suppressed
    }
    TimerStats &entry = stats[name];
    entry.wallSeconds += wallSeconds;
    entry.cpuSeconds += cpuSeconds > 0.0 ? cpuSeconds : 0.0;
    ++entry.calls;
}

void TimerRegistry::setEnabled(const std::string &name, bool isEnabled) {
    std::lock_guard<std::mutex> lock(mutex);
    enabled[name] = isEnabled;
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
}

void TimerRegistry::report() const {
    const auto all = snapshot();
    if (all.empty()) {
        return;
    }
    ktReportTable table(
        fmt::format("Timings (wall {:.3f}s, cpu {:.3f}s, {:.2f}x parallelism)", totalWallSeconds(),
                    totalCpuSeconds(),
                    totalWallSeconds() > 0.0 ? totalCpuSeconds() / totalWallSeconds() : 0.0));
    table.setHeaders({"phase", "wall", "cpu", "calls", "cpu/wall"});
    for (const auto &[name, entry] : all) {
        table.addRow({name, fmt::format("{:.3f}s", entry.wallSeconds),
                      fmt::format("{:.3f}s", entry.cpuSeconds), std::to_string(entry.calls),
                      fmt::format("{:.2f}x", entry.parallelism())});
    }
    table.emit();
}

ScopedTimer::ScopedTimer(std::string name)
    : timerName(std::move(name)), start(std::chrono::steady_clock::now()), cpuStart(std::clock()) {}

ScopedTimer::~ScopedTimer() {
    TimerRegistry::instance().record(timerName, elapsedSeconds(), cpuElapsedSeconds());
}

void ScopedTimer::lap() {
    const double wall = elapsedSeconds();
    const double cpu = cpuElapsedSeconds();
    start = std::chrono::steady_clock::now();
    cpuStart = std::clock();
    TimerRegistry::instance().record(timerName, wall, cpu);
}

double ScopedTimer::elapsedSeconds() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now() - start).count();
}

double ScopedTimer::cpuElapsedSeconds() const {
    const std::clock_t now = std::clock();
    if (cpuStart == 0 || now == static_cast<std::clock_t>(-1)) {
        return 0.0;
    }
    return static_cast<double>(now - cpuStart) / static_cast<double>(CLOCKS_PER_SEC);
}

const std::string &ScopedTimer::name() const {
    return timerName;
}

}  // namespace ktplace
