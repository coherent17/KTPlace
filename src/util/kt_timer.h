/**
 * @file kt_timer.h
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
 *                      many short intervals and report one summary line.
 *
 * All durations use `std::chrono::steady_clock`, which is monotonic and so
 * immune to wall-clock adjustments. Human-readable timestamps (in the log)
 * use the system clock instead, as the logger already does.
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

#ifndef KT_TIMER_H
#define KT_TIMER_H

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

/// Monotonic seconds from an arbitrary epoch; only differences are meaningful.
[[nodiscard]] double monotonicSeconds();

/// Accumulated statistics for one timer name.
struct TimerStats {
    double totalSeconds = 0.0;  ///< sum of all recorded intervals
    double minSeconds = 0.0;    ///< shortest interval seen
    double maxSeconds = 0.0;    ///< longest interval seen
    std::size_t calls = 0;      ///< number of recorded intervals
};

/// Process-wide collection of named timer totals.
class TimerRegistry {
public:
    /// @return the single registry
    static TimerRegistry &instance();

    TimerRegistry(const TimerRegistry &) = delete;
    TimerRegistry &operator=(const TimerRegistry &) = delete;

    /// Add one interval to @p name.
    void record(std::string name, double seconds);

    /**
     * @brief Enable or disable recording for @p name.
     *
     * Disabled timers still measure, but discard their result, which keeps
     * hot loops free of map and mutex traffic. Recording is enabled by
     * default.
     */
    void setEnabled(const std::string &name, bool enabled);

    /// @return true when @p name is currently recorded
    [[nodiscard]] bool isEnabled(const std::string &name) const;

    /// @return statistics for @p name, or nullptr when never recorded
    [[nodiscard]] const TimerStats *find(const std::string &name) const;

    /// @return a consistent copy of all statistics, ordered by name
    [[nodiscard]] std::vector<std::pair<std::string, TimerStats>> snapshot() const;

    /// @return summed seconds across every timer
    [[nodiscard]] double totalSeconds() const;

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

    /// @return seconds since construction or the last `lap()`
    [[nodiscard]] double elapsedSeconds() const;

    /// @return the timer's registry name
    [[nodiscard]] const std::string &name() const;

private:
    std::string timerName;
    std::chrono::steady_clock::time_point start;
};

}  // namespace ktplace

#endif  // KT_TIMER_H
