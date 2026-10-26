#include "util/kt_log.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iostream>

namespace ktplace {

std::string getLocalDateTime() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm);
    return buf;
}

const char *Logger::getLevelTag(Level tag) {
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

const char *Logger::getLevelColor(Level tag) {
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

Logger &Logger::getInstance() {
    static Logger logger;
    return logger;
}

Logger::~Logger() {
    shutdown();
}

void Logger::configure(std::string logFilePath, bool verbose) {
    const std::scoped_lock lock(mutex);
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

void Logger::setSink(Sink newSink) {
    const std::scoped_lock lock(mutex);
    sink = std::move(newSink);
}

void Logger::fail(const std::string &message) {
    emit(Level::Fatal, message);
    if (throwFatal) {
        throw FatalError(message);
    }
    shutdown();
    std::exit(EXIT_FAILURE);
}

void Logger::emit(Level level, const std::string &message) {
    const bool isTrace = (level == Level::Trace);
    const bool flagged = level == Level::Warning || level == Level::Fatal;

    std::string body = message;
    if (body.size() > maxRecordBytes) {
        body.resize(maxRecordBytes);
        body += " ...[truncated]";
    }

    const std::scoped_lock lock(mutex);

    const std::string prefix = '[' + step + "] ";
    const std::string tag = flagged ? std::string(getLevelTag(level)) + ": " : std::string();
    if (traceFile.is_open()) {
        traceFile << prefix << tag << body << '\n';
        traceFile.flush();
    }
    if (!isTrace && file.is_open()) {
        file << prefix << tag << body << '\n';
        file.flush();
    }
    if (!isTrace || verboseEnabled) {
        if (flagged) {
            std::cerr << prefix << getLevelColor(level) << tag << "\033[0m" << body << '\n';
        } else {
            std::cerr << prefix << body << '\n';
        }
    }
    if (sink) {
        sink(level, prefix + tag + body);
    }
}

std::string Logger::getCurrentStep() const {
    const std::scoped_lock lock(mutex);
    return step;
}

void Logger::beginStep(std::string name) {
    const std::size_t width = std::max<std::size_t>(60, name.size() + 4);
    const std::size_t inside = width - 2;
    const std::size_t left = (inside - name.size()) / 2;
    const std::string rule(width, '#');
    const std::string title =
        '#' + std::string(left, ' ') + name + std::string(inside - left - name.size(), ' ') + '#';
    const std::scoped_lock lock(mutex);
    step = std::move(name);
    for (std::ostream *out :
         {static_cast<std::ostream *>(&file), static_cast<std::ostream *>(&traceFile),
          static_cast<std::ostream *>(&std::cerr)}) {
        if (out == &file && !file.is_open()) {
            continue;
        }
        if (out == &traceFile && !traceFile.is_open()) {
            continue;
        }
        *out << rule << '\n' << title << '\n' << rule << '\n';
        out->flush();
    }
}

void Logger::shutdown() {
    const std::scoped_lock lock(mutex);
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
