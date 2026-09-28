/**
 * @file test_util.cc
 * @brief Tests for the logging and timing utilities
 *
 * The logger and the timer registry are process-wide singletons, so these tests
 * are about observable behaviour -- what lands in the transcript, what reaches
 * the console, and what a summary reports -- rather than internal state.
 *
 * The two sinks are checked separately, because they are deliberately different:
 * the transcript is plain text so it can be grepped and diffed, and only the
 * console view is coloured. A test that only looked at one of them would pass
 * even if the other regressed, and the regression that matters most here is a
 * warning that is invisible in a log file.
 */

#define BOOST_TEST_MODULE ktplace_util
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

using namespace ktplace;
namespace fs = std::filesystem;

namespace {

/// Redirect `std::cerr` into a string for the lifetime of the object.
class CaptureCerr {
public:
    CaptureCerr() : original_(std::cerr.rdbuf(buffer_.rdbuf())) {
    }
    ~CaptureCerr() {
        std::cerr.rdbuf(original_);
    }
    CaptureCerr(const CaptureCerr &) = delete;
    CaptureCerr &operator=(const CaptureCerr &) = delete;

    [[nodiscard]] std::string str() const {
        return buffer_.str();
    }

private:
    std::ostringstream buffer_;
    std::streambuf *original_;
};

/// A transcript file that removes itself, so a failing test leaves nothing behind.
class TempLog {
public:
    TempLog() {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("ktplace_log_" + std::to_string(++counter) + ".log");
    }
    ~TempLog() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempLog(const TempLog &) = delete;
    TempLog &operator=(const TempLog &) = delete;

    [[nodiscard]] std::string str() const {
        return path_.string();
    }

    /// @return the transcript's contents, or "" when it was never written
    [[nodiscard]] std::string contents() const {
        std::ifstream in(path_);
        if (!in.is_open()) {
            return {};
        }
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

private:
    std::filesystem::path path_;
};

/// SGR yellow, as the warning level uses.
constexpr const char *kYellow = "\033[1;33m";
/// SGR red, as the fatal level uses.
constexpr const char *kRed = "\033[1;31m";
/// SGR reset.
constexpr const char *kReset = "\033[0m";

}  // namespace

BOOST_AUTO_TEST_CASE(echo_reaches_both_sinks_without_colour) {
    TempLog log;
    ktlog.configure(log.str(), false);

    {
        CaptureCerr cap;
        ktlog.echo("plain record {}", 42);
        BOOST_TEST(cap.str().find("plain record 42") != std::string::npos);
        // An ordinary record is uncoloured: a normal transcript should read as
        // plain text, and escape codes in a log are noise at best.
        BOOST_TEST(cap.str().find(kYellow) == std::string::npos);
        BOOST_TEST(cap.str().find(kRed) == std::string::npos);
    }
    BOOST_TEST(log.contents().find("plain record 42") != std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(warning_is_yellow_on_console_and_plain_in_the_transcript) {
    TempLog log;
    ktlog.configure(log.str(), false);

    std::string console;
    {
        CaptureCerr cap;
        ktlog.warning("cell {} is taller than one row", 7);
        console = cap.str();
    }

    // Yellow on the console, with the level named, so it cannot be mistaken for
    // an ordinary line while the run is still going.
    BOOST_TEST(console.find(kYellow) != std::string::npos);
    BOOST_TEST(console.find("warning") != std::string::npos);
    BOOST_TEST(console.find(kReset) != std::string::npos);
    BOOST_TEST(console.find("cell 7 is taller than one row") != std::string::npos);

    // Plain in the file: tagged, and with no escape bytes at all. A log that
    // cannot be grepped is a log nobody reads.
    const std::string file = log.contents();
    BOOST_TEST(file.find("[warning]") != std::string::npos);
    BOOST_TEST(file.find("cell 7 is taller than one row") != std::string::npos);
    BOOST_TEST(file.find('\033') == std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(warning_accepts_a_preassembled_message) {
    // The overload for a runtime string, which cannot be a format string -- an
    // exception's what(), for instance. It must reach the sinks unchanged rather
    // than being treated as a format string with a stray brace in it.
    TempLog log;
    ktlog.configure(log.str(), false);

    const std::string message = "malformed {brace} in input";
    std::string console;
    {
        CaptureCerr cap;
        ktlog.warning(std::string_view(message));
        console = cap.str();
    }
    BOOST_TEST(console.find(message) != std::string::npos);
    BOOST_TEST(log.contents().find(message) != std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(fatal_is_red_and_exits) {
    // fatal() ends the process on purpose, so it is checked the only way it can
    // be: in a forked child, reading back what that child wrote to both sinks.
    // Fork rather than exec so no second binary and no run-time compilation is
    // needed, and so the child exercises this exact build of the logger.
    TempLog log;
    const std::string transcript = log.str();

    int channel[2];
    BOOST_REQUIRE_EQUAL(::pipe(channel), 0);

    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(channel[0]);
        ::dup2(channel[1], STDERR_FILENO);
        ::close(channel[1]);
        ktlog.configure(transcript, false);
        ktlog.fatal("cannot open {}", "x.nodes");
        std::fflush(nullptr);
        ::_exit(EXIT_SUCCESS);  // only reached when fatal() did NOT terminate
    }

    ::close(channel[1]);
    std::string console;
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        console.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(channel[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    BOOST_REQUIRE(WIFEXITED(status));
    // A non-zero exit code is the contract: the run must not look successful.
    BOOST_REQUIRE(WEXITSTATUS(status) != 0);

    // Red on the console, which is the sink a person reads when a run breaks.
    BOOST_TEST(console.find(kRed) != std::string::npos);
    BOOST_TEST(console.find("fatal") != std::string::npos);
    BOOST_TEST(console.find("cannot open x.nodes") != std::string::npos);

    // Plain in the transcript, tagged, no escape bytes.
    const std::string file = log.contents();
    BOOST_TEST(file.find("[fatal]") != std::string::npos);
    BOOST_TEST(file.find("cannot open x.nodes") != std::string::npos);
    BOOST_TEST(file.find('\033') == std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(record_count_includes_warnings) {
    TempLog log;
    ktlog.configure(log.str(), false);
    const std::size_t before = ktlog.recordCount();
    {
        CaptureCerr cap;
        ktlog.echo("one");
        ktlog.warning("two");
    }
    BOOST_TEST(ktlog.recordCount() == before + 2);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(a_scoped_timer_records_its_interval_on_scope_exit) {
    TimerRegistry::instance().reset();
    {
        const ScopedTimer timer("unit-timer-a");
        BOOST_TEST(timer.name() == "unit-timer-a");
        BOOST_TEST(timer.elapsedSeconds() >= 0.0);
    }
    const TimerStats *stats = TimerRegistry::instance().find("unit-timer-a");
    BOOST_REQUIRE(stats != nullptr);
    BOOST_TEST(stats->calls == 1U);
    BOOST_TEST(stats->wallSeconds >= 0.0);
    BOOST_TEST(stats->cpuSeconds >= 0.0);
}

BOOST_AUTO_TEST_CASE(repeated_intervals_accumulate_under_one_name) {
    // A loop of short intervals reporting as one total is the whole point of the
    // registry, so the accumulation is what needs a test.
    TimerRegistry::instance().reset();
    for (int i = 0; i < 5; ++i) {
        const ScopedTimer timer("unit-timer-b");
    }
    const TimerStats *stats = TimerRegistry::instance().find("unit-timer-b");
    BOOST_REQUIRE(stats != nullptr);
    BOOST_TEST(stats->calls == 5U);
}

BOOST_AUTO_TEST_CASE(timer_totals_cover_every_recorded_name) {
    TimerRegistry::instance().reset();
    {
        const ScopedTimer a("unit-timer-c");
    }
    {
        const ScopedTimer b("unit-timer-d");
    }
    TimerRegistry &reg = TimerRegistry::instance();
    const auto snapshot = reg.snapshot();
    BOOST_TEST(snapshot.size() == 2U);
    // snapshot() is ordered by name and is a copy taken under the lock, so it is
    // the only safe way to read the registry while workers are still running.
    BOOST_TEST(snapshot[0].first == "unit-timer-c");
    BOOST_TEST(snapshot[1].first == "unit-timer-d");
    BOOST_TEST(reg.totalWallSeconds() >= 0.0);
    BOOST_TEST(reg.totalCpuSeconds() >= 0.0);
    BOOST_TEST(reg.isEnabled("unit-timer-c"));
}

BOOST_AUTO_TEST_CASE(reset_clears_statistics_but_keeps_names_known) {
    TimerRegistry::instance().reset();
    {
        const ScopedTimer timer("unit-timer-e");
    }
    BOOST_TEST(TimerRegistry::instance().find("unit-timer-e") != nullptr);
    TimerRegistry::instance().reset();
    BOOST_TEST(TimerRegistry::instance().find("unit-timer-e") == nullptr);
    // A name stays enabled after a reset, so a timer that is created later is
    // still recorded rather than silently dropped.
    BOOST_TEST(TimerRegistry::instance().isEnabled("unit-timer-e"));
}

BOOST_AUTO_TEST_CASE(parallelism_of_an_unrun_timer_is_zero_not_a_division) {
    const TimerStats fresh;
    BOOST_TEST(fresh.parallelism() == 0.0);
    const TimerStats serial{1.0, 1.0, 1};
    BOOST_TEST(serial.parallelism() == 1.0);
    const TimerStats parallel{1.0, 4.0, 1};
    BOOST_TEST(parallel.parallelism() == 4.0);
}
