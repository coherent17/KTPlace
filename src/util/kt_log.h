#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ktplace {

/// Thrown by Logger::fatal once the fatal record has been written.
///
/// A fatal error ends the current operation, not the process: the CLI's main()
/// catches it and exits with EXIT_FAILURE, while an embedding host (the GUI)
/// catches it, reports it, and carries on with the logger still open.
///
/// It deliberately does NOT derive from std::exception. Library code catches
/// `const std::exception &` in several places to skip one malformed input record
/// and keep going (the bookshelf and LEF/DEF adaptors around std::stod and
/// db->addPin, the visualizer around loading a frame, PlotOptions around parsing
/// a number). A fatal raised beneath any of those, now or after a later change,
/// would be silently swallowed and the run would continue past an error that was
/// meant to stop it. Outside the std::exception hierarchy only an explicit
/// `catch (const FatalError &)` or a `catch (...)` stops it, and the only
/// `catch (...)` in src/ is main()'s, which lets it through. TBB carries any
/// exception type out of a parallel region, so nothing is lost there either.
class FatalError {
public:
    explicit FatalError(std::string message) : msg(std::move(message)) {}

    /// The fatal record's text, as passed to fatal() (before log truncation).
    [[nodiscard]] const std::string &message() const noexcept {
        return msg;
    }
    /// Spelled like std::exception::what() for callers used to that interface.
    [[nodiscard]] const char *what() const noexcept {
        return msg.c_str();
    }

private:
    std::string msg;
};

/// Severity of a log record, as passed to a sink.
enum class LogLevel { Echo, Trace, Warning, Fatal };

class Logger {
public:
    static constexpr std::size_t kMaxRecordBytes = 8000;

    static Logger &instance();

    ~Logger();

    Logger(const Logger &) = delete;
    Logger &operator=(const Logger &) = delete;

    void configure(std::string logFilePath, bool verbose);

    [[nodiscard]] bool verbose() const;

    [[nodiscard]] const std::string &logFilePath() const;

    [[nodiscard]] const std::string &traceFilePath() const;

    [[nodiscard]] static std::string tracePathFor(const std::string &logFilePath);

    [[nodiscard]] std::size_t recordCount() const;

    template <typename... Args>
    void echo(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(LogLevel::Echo, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void trace(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(LogLevel::Trace, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(LogLevel::Warning, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    /// Writes a fatal record and throws FatalError carrying its message. The log
    /// stays open: ending the process, if anything should, is the caller's job
    /// (see main() in kt_place.cc).
    template <typename... Args>
    [[noreturn]] void fatal(fmt::format_string<Args...> fmtStr, Args &&...args) {
        fatal(std::string_view(fmt::format(fmtStr, std::forward<Args>(args)...)));
    }

    void echo(std::string_view message) {
        emit(LogLevel::Echo, std::string(message));
    }
    void trace(std::string_view message) {
        emit(LogLevel::Trace, std::string(message));
    }
    void warning(std::string_view message) {
        emit(LogLevel::Warning, std::string(message));
    }
    [[noreturn]] void fatal(std::string_view message) {
        std::string text(message);
        emit(LogLevel::Fatal, text);
        throw FatalError(std::move(text));
    }

    void shutdown();

    /// The tag a record carries in the transcript: "echo", "trace", "warning",
    /// "fatal".
    [[nodiscard]] static const char *levelTag(LogLevel level);

    // ---- Sinks ------------------------------------------------------------
    //
    // A sink receives every record (trace included, whatever `verbose` says, so
    // the sink does its own filtering) with the same text the transcript gets,
    // after the file and stderr writes. stdout is still never written to.
    //
    // Threading contract:
    //  - A sink runs on the thread that emitted the record, which is often a TBB
    //    worker. It must be safe to call concurrently from several threads, and a
    //    GUI sink should queue the record to its own thread rather than touch
    //    widgets.
    //  - It runs OUTSIDE the logger's write mutex. Holding that mutex would make
    //    a slow sink stall every thread that logs, and a sink that logs (or calls
    //    anything that does) would deadlock on the non-recursive mutex. The cost
    //    is ordering: records from different threads may reach a sink in a
    //    different order than the transcript has them. Records from one thread
    //    arrive in the order that thread emitted them.
    //  - A record emitted from inside a sink, on that sink's thread, goes to the
    //    file and stderr but is not delivered to sinks again, so a logging sink
    //    cannot recurse without bound.
    //  - A sink that throws is contained: the exception is dropped, the other
    //    sinks and the transcript are unaffected, and the first failure of each
    //    sink is reported once as a warning.
    //  - addSink and removeSink may be called from any thread, including from
    //    inside a sink. Once removeSink returns, the sink is not running and will
    //    not be called again, so whatever it captured can be destroyed. The one
    //    exception is removeSink called from inside a sink callback: it cannot
    //    wait for in-flight calls (it may be one of them), so it only guarantees
    //    no new calls start. A sink must therefore not block waiting on a thread
    //    that may be inside removeSink, or the two wait for each other.
    using SinkId = std::size_t;
    using Sink = std::function<void(LogLevel, std::string_view)>;

    /// Registers `sink`; returns the id that removes it. An empty function is
    /// accepted and never called.
    SinkId addSink(Sink sink);

    /// Unregisters a sink. An unknown or already removed id is ignored.
    void removeSink(SinkId id);

private:
    struct SinkSlot;

    Logger() = default;

    void emit(LogLevel level, const std::string &message);
    void dispatchToSinks(LogLevel level, std::string_view body);
    static const char *levelColor(LogLevel level);
    void closeFiles();

    std::mutex sinkMutex;
    std::condition_variable sinkIdle;  // a slot's in-flight count reached zero
    std::vector<std::shared_ptr<SinkSlot>> sinks;
    std::atomic<std::size_t> sinkCount{0};  // lets emit skip sinkMutex when there are none
    SinkId nextSinkId = 1;

    mutable std::mutex mutex;
    std::string path;
    std::string tracePath;
    std::ofstream file;
    std::ofstream traceFile;
    bool verboseEnabled = false;
    bool reportedOpenFailure = false;
    std::size_t records = 0;
};

inline Logger &ktlog = Logger::instance();

}  // namespace ktplace
