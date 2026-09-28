/**
 * @file kt_scopedTimer.h
 * @brief Elapsed-time measurement for KTPlace
 *
 * This is a stopwatch facility only: it answers "how long did this take?".
 * It is deliberately *not* a circuit-delay calculator -- cell delay, net delay
 * and slack need a timing graph plus standard-cell libraries and must be
 * recomputed as the placement moves, so they belong in a separate timing
 * engine. Such an engine can use `ScopedTimer` to report its own cost, which
 * is the only intended overlap between the two.
 *
 * Two pieces:
 *   - `ScopedTimer`  : RAII stopwatch; records on scope exit (exception safe).
 *   - `TimerRegistry`: process-wide named totals, so a loop can accumulate
 *                      many short intervals and report one summary table.
 *
 * Each interval is recorded twice: wall-clock time, which is what the user
 * waits for, and processor time summed over all threads. The ratio is the
 * effective parallelism, which is the number worth watching in a parallel
 * solve. (A phase that is quietly serial shows a ratio near 1.0.)
 *
 * Wall durations use `std::chrono::steady_clock`, which is monotonic and so
 * immune to wall-clock adjustments; processor time uses `std::clock()`.
 * Human-readable timestamps in the log use the system clock, as the logger
 * already does.
 *
 * Usage:
 * @code
 *   using namespace ktplace;
 *   {
 *       ScopedTimer t("load");
 *       runLoad();
 *   }                                   // records on destruction
 *   for (int i = 0; i < n; ++i) {
 *       ScopedTimer lap("outer-iter");  // accumulates across iterations
 *       step();
 *   }
 *   TimerRegistry::instance().report(); // one summary block via ktlog
 * @endcode
 */

#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

/**
 * @brief Accumulated statistics for one timer name.
 *
 * Wall time is what the user waits for; CPU time is the processor time the
 * process consumed, summed over all threads. Their ratio is the effective
 * parallelism: near 1.0 means the work was serial, and a ratio of N means it
 * used roughly N cores on average.
 */
struct TimerStats {
    double wallSeconds = 0.0;  ///< summed wall-clock intervals
    double cpuSeconds = 0.0;   ///< summed processor time over all threads
    std::size_t calls = 0;     ///< number of recorded intervals

    /// @return cpuSeconds / wallSeconds, or 0.0 when no wall time accumulated
    [[nodiscard]] double parallelism() const {
        return wallSeconds > 0.0 ? cpuSeconds / wallSeconds : 0.0;
    }
};

/// Process-wide collection of named timer totals.
class TimerRegistry {
public:
    /// @return the single registry
    static TimerRegistry &instance();

    TimerRegistry(const TimerRegistry &) = delete;
    TimerRegistry &operator=(const TimerRegistry &) = delete;

    /// Add one interval to @p name.
    void record(std::string name, double wallSeconds, double cpuSeconds);

    /// @return true when @p name is currently recorded
    [[nodiscard]] bool isEnabled(const std::string &name) const;

    /// @return statistics for @p name, or nullptr when never recorded
    [[nodiscard]] const TimerStats *find(const std::string &name) const;

    /// @return a consistent copy of all statistics, ordered by name
    [[nodiscard]] std::vector<std::pair<std::string, TimerStats>> snapshot() const;

    /// @return summed wall-clock seconds across every timer
    [[nodiscard]] double totalWallSeconds() const;

    /// @return summed processor seconds across every timer
    [[nodiscard]] double totalCpuSeconds() const;

    /// Discard all statistics (enabled/disabled flags are kept).
    void reset();

    /// Emit a summary table through `ktlog` (one line per timer).
    void report() const;

private:
    TimerRegistry() = default;

    mutable std::mutex mutex;
    std::map<std::string, TimerStats> stats;
    std::map<std::string, bool> enabled;
};

/**
 * @brief RAII stopwatch that records its interval on destruction.
 *
 * A timer is safe to use from several threads for *measuring*, but recording
 * into the registry takes a mutex, so keep these out of tight parallel
 * regions unless the name is disabled.
 */
class ScopedTimer {
public:
    /// Start measuring under @p name.
    explicit ScopedTimer(std::string name);

    ScopedTimer(const ScopedTimer &) = delete;
    ScopedTimer &operator=(const ScopedTimer &) = delete;

    /// Record the interval measured so far and restart the stopwatch.
    void lap();

    /// Record the interval; called automatically on destruction.
    ~ScopedTimer();

    /// @return wall-clock seconds since construction or the last `lap()`
    [[nodiscard]] double elapsedSeconds() const;

    /// @return processor seconds consumed since construction or the last `lap()`
    [[nodiscard]] double cpuElapsedSeconds() const;

    /// @return the timer's registry name
    [[nodiscard]] const std::string &name() const;

private:
    std::string timerName;
    std::chrono::steady_clock::time_point start;
    std::clock_t cpuStart = 0;
};

}  // namespace ktplace
