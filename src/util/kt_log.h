/**
 * @file kt_log.h
 * @brief Single-sink logging facility for KTPlace
 *
 * All engine output funnels through the global `ktplace::ktlog` object so that
 * nothing writes to `std::cout` directly. Two sinks are used:
 *
 *   - a log file (`ktplace.log` by default, overridable with `--log-file`),
 *     which receives the full transcript including trace records;
 *   - `std::cerr`, which receives the user-facing `echo`/`fatal` records so an
 *     interactive run still shows progress. Nothing is written to stdout, so
 *     redirecting stdout stays clean and predictable.
 *
 * Messages are built with `fmt::format`; format strings are validated at
 * compile time through `fmt::format_string`.
 *
 * Usage:
 * @code
 *   using ktplace::ktlog;
 *   ktlog.echo("Loaded {} cells", n);
 *   ktlog.trace("step {} residual {:.6}", step, resid);  // needs -v
 *   ktlog.fatal("cannot open {}", path);                // writes and exits
 * @endcode
 */

#ifndef KT_LOG_H
#define KT_LOG_H

#include <fmt/format.h>

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ktplace {

/**
 * @brief Logger writing to a transcript file plus stderr.
 *
 * Calls are safe from multiple threads: each record is emitted under one
 * mutex, so output from TBB workers cannot interleave within a line. Copying
 * is disabled so the global `ktlog` instance below is the only logger.
 */
class Logger {
public:
    /// Maximum length of one record; longer records are truncated.
    static constexpr std::size_t kMaxRecordBytes = 8000;

    Logger() = default;
    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /**
     * @brief Open the transcript file and set the verbosity.
     *
     * Idempotent; calling it again reconfigures both sinks. An empty
     * @p logFilePath disables the file sink, leaving stderr only.
     *
     * @param logFilePath  transcript path, or "" to disable the file sink
     * @param verbose      when true, `trace` records are emitted
     */
    void configure(std::string logFilePath, bool verbose);

    /// Enable or disable `trace` output at runtime.
    void setVerbose(bool enabled);

    /// @return true when `trace` records are being emitted
    [[nodiscard]] bool verbose() const;

    /// @return active transcript path ("" when the file sink is disabled)
    [[nodiscard]] const std::string &logFilePath() const;

    /// @return number of records written so far
    [[nodiscard]] std::size_t recordCount() const;

    /// Write a user-facing record to the transcript and to stderr.
    template <typename... Args>
    void echo(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Echo, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    /// Write a diagnostic record; emitted only when verbose output is enabled.
    template <typename... Args>
    void trace(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Trace, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    /// Write an error record to the transcript and stderr, then exit(EXIT_FAILURE).
    template <typename... Args>
    [[noreturn]] void fatal(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Fatal, fmt::format(fmtStr, std::forward<Args>(args)...));
        shutdown();
        std::exit(EXIT_FAILURE);
    }

    // Overloads for records that are already assembled at run time (for
    // example an exception's what() string), which cannot be format strings.
    void echo(std::string_view message) {
        emit(Level::Echo, std::string(message));
    }
    void trace(std::string_view message) {
        emit(Level::Trace, std::string(message));
    }
    [[noreturn]] void fatal(std::string_view message) {
        emit(Level::Fatal, std::string(message));
        shutdown();
        std::exit(EXIT_FAILURE);
    }

    /// Flush and close the transcript (idempotent).
    void shutdown();

private:
    enum class Level { Echo, Trace, Fatal };

    void emit(Level level, const std::string &message);
    static const char *levelTag(Level level);

    mutable std::mutex mutex;
    std::string path;
    std::ofstream file;
    bool verboseEnabled = false;
    bool reportedOpenFailure = false;
    std::size_t records = 0;
};

/// Global logger instance used throughout KTPlace.
inline Logger ktlog;

}  // namespace ktplace

#endif  // KT_LOG_H
