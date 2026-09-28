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

#pragma once

#include <fmt/format.h>

#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
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

    /**
     * @brief The one and only logger.
     *
     * The constructor is private, so a second logger cannot be created by
     * accident; use the global `ktlog` object below.
     */
    static Logger &instance();

    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    /**
     * @brief Open the transcript file and set the verbosity.
     *
     * Idempotent; calling it again reconfigures both sinks. An empty
     * @p logFilePath disables the file sink, leaving stderr only.
     *
     * Trace output goes to a second file next to it, named by inserting
     * "_trace" before the extension ("ktplace.log" -> "ktplace_trace.log"),
     * and is only created when @p verbose is true.
     *
     * @param logFilePath  transcript path, or "" to disable the file sink
     * @param verbose      when true, open the trace file and emit `trace`
     */
    void configure(std::string logFilePath, bool verbose);

    /// @return true when `trace` records are being emitted
    [[nodiscard]] bool verbose() const;

    /// @return active transcript path ("" when the file sink is disabled)
    [[nodiscard]] const std::string &logFilePath() const;

    /// @return trace file path ("" when tracing is off)
    [[nodiscard]] const std::string &traceFilePath() const;

    /// @return the trace path @p logFilePath implies ("" for an empty input)
    [[nodiscard]] static std::string tracePathFor(const std::string &logFilePath);

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

    /**
     * @brief Write a warning to the transcript and stderr, in yellow.
     *
     * A warning is a condition the run recovered from or deliberately accepted:
     * it does not stop the flow, but it is not something to discover later from a
     * count in a table either. It goes to the transcript uncoloured, so a log
     * file is plain text and greppable, and to stderr in yellow, so the word
     * "warning" is visible while a run is still going.
     */
    template <typename... Args>
    void warning(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Warning, fmt::format(fmtStr, std::forward<Args>(args)...));
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
    void warning(std::string_view message) {
        emit(Level::Warning, std::string(message));
    }
    [[noreturn]] void fatal(std::string_view message) {
        emit(Level::Fatal, std::string(message));
        shutdown();
        std::exit(EXIT_FAILURE);
    }

    /// Flush and close the transcript (idempotent).
    void shutdown();

private:
    enum class Level { Echo, Trace, Warning, Fatal };

    Logger() = default;

    void emit(Level level, const std::string &message);
    static const char *levelTag(Level level);
    /// SGR colour for @p level on stderr, or "" for an uncoloured level.
    static const char *levelColor(Level level);
    void closeFiles();  // caller holds mutex

    mutable std::mutex mutex;
    std::string path;
    std::string tracePath;
    std::ofstream file;
    std::ofstream traceFile;
    bool verboseEnabled = false;
    bool reportedOpenFailure = false;
    std::size_t records = 0;
};

/// Global logger instance used throughout KTPlace.
inline Logger &ktlog = Logger::instance();

}  // namespace ktplace
