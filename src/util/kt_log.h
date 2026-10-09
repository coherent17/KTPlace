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

    template <typename... Args>
    [[noreturn]] void fatal(fmt::format_string<Args...> fmtStr, Args &&...args) {
        emit(Level::Fatal, fmt::format(fmtStr, std::forward<Args>(args)...));
        shutdown();
        std::exit(EXIT_FAILURE);
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
        emit(Level::Fatal, std::string(message));
        shutdown();
        std::exit(EXIT_FAILURE);
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