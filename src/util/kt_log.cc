#include "util/kt_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <iostream>

namespace ktplace {
namespace {

std::string timestamp() {
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const std::time_t secs = clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &secs);
#else
    localtime_r(&secs, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(millis));
    return buf;
}

}  // namespace

const char *Logger::levelTag(Level tag) {
    switch (tag) {
        case Level::Echo:
            return "echo";
        case Level::Trace:
            return "trace";
        case Level::Warning:
            return "warning";
        case Level::Fatal:
            return "fatal";
    }
    return "?";
}

const char *Logger::levelColor(Level tag) {
    switch (tag) {
        case Level::Warning:
            return "\033[1;33m";
        case Level::Fatal:
            return "\033[1;31m";
        case Level::Echo:
        case Level::Trace:
            break;
    }
    return "";
}

Logger &Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    shutdown();
}

void Logger::configure(std::string logFilePath, bool verbose) {
    std::lock_guard<std::mutex> lock(mutex);
    closeFiles();
    reportedOpenFailure = false;
    path = std::move(logFilePath);
    tracePath = tracePathFor(path);
    verboseEnabled = verbose;

    const auto openOne = [this](std::ofstream &stream, const std::string &target) {
        if (target.empty()) {
            return;
        }
        stream.open(target, std::ios::out | std::ios::trunc);
        if (!stream.is_open() && !reportedOpenFailure) {
            reportedOpenFailure = true;
            std::cerr << "[ktlog] warning: cannot open log file '" << target
                      << "'; continuing with stderr only\n";
        }
    };
    openOne(file, path);
    openOne(traceFile, tracePath);
}

std::string Logger::tracePathFor(const std::string &logFilePath) {
    if (logFilePath.empty()) {
        return {};
    }
    const std::size_t dot = logFilePath.rfind('.');
    const std::size_t slash = logFilePath.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return logFilePath + "_trace";
    }
    return logFilePath.substr(0, dot) + "_trace" + logFilePath.substr(dot);
}

const std::string &Logger::traceFilePath() const {
    return tracePath;
}

bool Logger::verbose() const {
    std::lock_guard<std::mutex> lock(mutex);
    return verboseEnabled;
}

const std::string &Logger::logFilePath() const {
    return path;
}

std::size_t Logger::recordCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return records;
}

void Logger::emit(Level level, const std::string &message) {
    const bool isTrace = (level == Level::Trace);

    std::string body = message;
    if (body.size() > kMaxRecordBytes) {
        body.resize(kMaxRecordBytes);
        body += " ...[truncated]";
    }

    std::lock_guard<std::mutex> lock(mutex);
    ++records;

    if (isTrace) {
        if (traceFile.is_open()) {
            traceFile << timestamp() << " [trace] " << body << '\n';
            traceFile.flush();
        }
    } else if (file.is_open()) {
        file << timestamp() << " [" << levelTag(level) << "] " << body << '\n';
        file.flush();
    }

    if (!isTrace || verboseEnabled) {
        const char *color = levelColor(level);
        if (color[0] != '\0') {
            std::cerr << color << '[' << levelTag(level) << "] \033[0m " << body << '\n';
        } else {
            std::cerr << body << '\n';
        }
    }
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    closeFiles();
}

void Logger::closeFiles() {
    for (std::ofstream *stream : {&file, &traceFile}) {
        if (stream->is_open()) {
            stream->flush();
            stream->close();
        }
    }
}

}  // namespace ktplace