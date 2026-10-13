#pragma once

#include <cstddef>
#include <cstdlib>
#include <fmt/format.h>
#include <fstream>
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
        emit(Level::Echo, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void trace(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Trace, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Warning, fmt::format(fmtStr, std::forward<Args>(args)...));
    }

    /// Writes a fatal record and throws FatalError carrying its message. The log
    /// stays open: ending the process, if anything should, is the caller's job
    /// (see main() in kt_place.cc).
    template <typename... Args>
    [[noreturn]] void fatal(fmt::format_string<Args...> fmtStr, Args &&...args) {
        fatal(std::string_view(fmt::format(fmtStr, std::forward<Args>(args)...)));
    }

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
        std::string text(message);
        emit(Level::Fatal, text);
        throw FatalError(std::move(text));
    }

    void shutdown();

private:
    enum class Level { Echo, Trace, Warning, Fatal };

    Logger() = default;

    void emit(Level level, const std::string &message);
    static const char *levelTag(Level level);
    static const char *levelColor(Level level);
    void closeFiles();

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
