/**
 * @file kt_timer.cc
 * @brief Implementation of the KTPlace elapsed-time measurement
 */

#include "util/kt_timer.h"
#include "util/kt_log.h"
#include <limits>

namespace ktplace {

double monotonicSeconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

TimerRegistry &TimerRegistry::instance() {
    static TimerRegistry registry;
    return registry;
}

void TimerRegistry::record(std::string name, double seconds) {
    if (seconds < 0.0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex);
    auto it = enabled.find(name);
    if (it != enabled.end() && !it->second) {
        return;  // measuring continues, recording is suppressed
    }
    TimerStats &entry = stats[name];
    if (entry.calls == 0) {
        entry.minSeconds = seconds;
        entry.maxSeconds = seconds;
    } else {
        entry.minSeconds = std::min(entry.minSeconds, seconds);
        entry.maxSeconds = std::max(entry.maxSeconds, seconds);
    }
    entry.totalSeconds += seconds;
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

double TimerRegistry::totalSeconds() const {
    std::lock_guard<std::mutex> lock(mutex);
    double sum = 0.0;
    for (const auto &[name, entry] : stats) {
        sum += entry.totalSeconds;
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
    const double grand = totalSeconds();
    ktlog.echo("Timings (total {:.3f}s):", grand);
    for (const auto &[name, entry] : all) {
        ktlog.echo("  {:<22} {:>10.3f}s  x{:<6} min {:.3f}s  max {:.3f}s", name, entry.totalSeconds,
                   entry.calls, entry.minSeconds, entry.maxSeconds);
    }
}

ScopedTimer::ScopedTimer(std::string name)
    : timerName(std::move(name)), start(std::chrono::steady_clock::now()) {}

ScopedTimer::~ScopedTimer() {
    TimerRegistry::instance().record(timerName, elapsedSeconds());
}

void ScopedTimer::lap() {
    const double delta = elapsedSeconds();
    start = std::chrono::steady_clock::now();
    TimerRegistry::instance().record(timerName, delta);
}

double ScopedTimer::elapsedSeconds() const {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now() - start).count();
}

const std::string &ScopedTimer::name() const {
    return timerName;
}

}  // namespace ktplace
