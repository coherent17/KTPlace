#include "util/kt_log.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <exception>
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

// How many sink dispatches are running on this thread. Non-zero means the
// current record was emitted from inside a sink: it is not dispatched again, and
// removeSink must not wait for in-flight calls (this thread is one of them).
thread_local int sinkDispatchDepth = 0;

}  // namespace

struct Logger::SinkSlot {
    SinkId id = 0;
    Sink fn;
    std::size_t inFlight = 0;  // guarded by Logger::sinkMutex
    std::atomic<bool> reportedThrow{false};
};

const char *Logger::levelTag(LogLevel tag) {
    switch (tag) {
        case LogLevel::Echo:
            return "echo";
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Warning:
            return "warning";
        case LogLevel::Fatal:
            return "fatal";
    }
    return "?";
}

const char *Logger::levelColor(LogLevel tag) {
    switch (tag) {
        case LogLevel::Warning:
            return "\033[1;33m";
        case LogLevel::Fatal:
            return "\033[1;31m";
        case LogLevel::Echo:
        case LogLevel::Trace:
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

void Logger::emit(LogLevel level, const std::string &message) {
    const bool isTrace = (level == LogLevel::Trace);

    std::string body = message;
    if (body.size() > kMaxRecordBytes) {
        body.resize(kMaxRecordBytes);
        body += " ...[truncated]";
    }

    {
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

    // Outside the write mutex on purpose; see the threading contract in kt_log.h.
    // Sink exceptions are contained inside dispatchToSinks; this only catches an
    // allocation failure taking the snapshot, which must not turn a fatal()'s
    // FatalError into std::bad_alloc or make logging throw.
    if (sinkCount.load(std::memory_order_acquire) != 0 && sinkDispatchDepth == 0) {
        try {
            dispatchToSinks(level, body);
        } catch (...) {
        }
    }
}

void Logger::dispatchToSinks(LogLevel level, std::string_view body) {
    std::vector<std::shared_ptr<SinkSlot>> snapshot;
    {
        std::lock_guard<std::mutex> lock(sinkMutex);
        snapshot = sinks;
        for (const std::shared_ptr<SinkSlot> &slot : snapshot) {
            ++slot->inFlight;
        }
    }

    ++sinkDispatchDepth;
    for (const std::shared_ptr<SinkSlot> &slot : snapshot) {
        std::string failure;
        try {
            slot->fn(level, body);
        } catch (const FatalError &e) {
            failure = "FatalError: " + e.message();
        } catch (const std::exception &e) {
            failure = e.what();
        } catch (...) {
            failure = "unknown exception";
        }
        if (!failure.empty() && !slot->reportedThrow.exchange(true)) {
            // Still inside the dispatch, so this reaches the transcript and
            // stderr but not the sinks, and cannot loop.
            try {
                emit(LogLevel::Warning,
                     fmt::format("log sink {} threw ({}); further failures of this sink are "
                                 "not reported",
                                 slot->id, failure));
            } catch (...) {  // nothing sensible left to do; logging must go on
            }
        }
    }
    --sinkDispatchDepth;

    {
        std::lock_guard<std::mutex> lock(sinkMutex);
        for (const std::shared_ptr<SinkSlot> &slot : snapshot) {
            --slot->inFlight;
        }
    }
    sinkIdle.notify_all();
}

Logger::SinkId Logger::addSink(Sink sink) {
    std::lock_guard<std::mutex> lock(sinkMutex);
    const SinkId id = nextSinkId++;
    if (!sink) {
        return id;  // nothing to call; the id is still valid to remove
    }
    auto slot = std::make_shared<SinkSlot>();
    slot->id = id;
    slot->fn = std::move(sink);
    sinks.push_back(std::move(slot));
    sinkCount.store(sinks.size(), std::memory_order_release);
    return id;
}

void Logger::removeSink(SinkId id) {
    std::shared_ptr<SinkSlot> removed;
    std::unique_lock<std::mutex> lock(sinkMutex);
    for (auto it = sinks.begin(); it != sinks.end(); ++it) {
        if ((*it)->id == id) {
            removed = std::move(*it);
            sinks.erase(it);
            break;
        }
    }
    if (!removed) {
        return;
    }
    sinkCount.store(sinks.size(), std::memory_order_release);
    if (sinkDispatchDepth == 0) {
        sinkIdle.wait(lock, [&removed] {
            return removed->inFlight == 0;
        });
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
