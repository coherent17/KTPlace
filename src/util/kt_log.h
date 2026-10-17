#pragma once

#include <atomic>
#include <cstddef>
#include <format>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ktplace {

class FatalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Logger {
public:
    enum class Level { Echo, Trace, Warning, Fatal };
    using Sink = std::function<void(Level, std::string_view)>;

    static constexpr std::size_t maxRecordBytes = 8000;

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

    // Called with the logger locked, so a sink must not log.
    void setSink(Sink sink);

    void throwOnFatal(bool enabled) {
        throwFatal = enabled;
    }

    template <typename... Args>
    void echo(std::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Echo, std::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void trace(std::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Trace, std::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    void warning(std::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Warning, std::format(fmtStr, std::forward<Args>(args)...));
    }

    template <typename... Args>
    [[noreturn]] void fatal(std::format_string<Args...> fmtStr, Args &&...args) {
        fail(std::format(fmtStr, std::forward<Args>(args)...));
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
        fail(std::string(message));
    }

    /// Start a step of the run: print its banner and prefix every later line
    /// with its name until the next step begins.
    void beginStep(std::string name);
    [[nodiscard]] std::string currentStep() const;

    void shutdown();

private:
    Logger() = default;

    void emit(Level level, const std::string &message);
    [[noreturn]] void fail(const std::string &message);
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
    std::string step = "ktplace";
    std::size_t records = 0;
    Sink sink;
    std::atomic<bool> throwFatal = false;
};

inline Logger &ktlog = Logger::instance();

/// The local date and time, e.g. "2026-10-16 13:50:04 CST".
[[nodiscard]] std::string localDateTime();

}  // namespace ktplace
