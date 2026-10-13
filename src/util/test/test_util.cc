#define BOOST_TEST_MODULE ktplace_util
#define BOOST_TEST_DYN_LINK
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <boost/test/unit_test.hpp>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

using namespace ktplace;

namespace {

class CaptureCerr {
public:
    CaptureCerr() : original(std::cerr.rdbuf(buffer.rdbuf())) {}
    ~CaptureCerr() {
        std::cerr.rdbuf(original);
    }
    CaptureCerr(const CaptureCerr &) = delete;
    CaptureCerr &operator=(const CaptureCerr &) = delete;

    [[nodiscard]] std::string str() const {
        return buffer.str();
    }

private:
    std::ostringstream buffer;
    std::streambuf *original;
};

class TempLog {
public:
    TempLog() {
        static int counter = 0;
        path = std::filesystem::temp_directory_path() /
               ("ktplace_log_" + std::to_string(++counter) + ".log");
    }
    ~TempLog() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    TempLog(const TempLog &) = delete;
    TempLog &operator=(const TempLog &) = delete;

    [[nodiscard]] std::string str() const {
        return path.string();
    }

    [[nodiscard]] std::string contents() const {
        std::ifstream in(path);
        if (!in.is_open()) {
            return {};
        }
        std::ostringstream out;
        out << in.rdbuf();
        return out.str();
    }

private:
    std::filesystem::path path;
};

constexpr const char *kYellow = "\033[1;33m";
constexpr const char *kRed = "\033[1;31m";
constexpr const char *kReset = "\033[0m";

}  // namespace

BOOST_AUTO_TEST_CASE(echo_reaches_both_sinks_without_colour) {
    TempLog log;
    ktlog.configure(log.str(), false);

    {
        CaptureCerr cap;
        ktlog.echo("plain record {}", 42);
        BOOST_TEST(cap.str().find("plain record 42") != std::string::npos);
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

    BOOST_TEST(console.find(kYellow) != std::string::npos);
    BOOST_TEST(console.find("warning") != std::string::npos);
    BOOST_TEST(console.find(kReset) != std::string::npos);
    BOOST_TEST(console.find("cell 7 is taller than one row") != std::string::npos);

    const std::string file = log.contents();
    BOOST_TEST(file.find("[warning]") != std::string::npos);
    BOOST_TEST(file.find("cell 7 is taller than one row") != std::string::npos);
    BOOST_TEST(file.find('\033') == std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(warning_accepts_a_preassembled_message) {
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

BOOST_AUTO_TEST_CASE(fatal_throws_a_fatal_error_carrying_the_message) {
    TempLog log;
    ktlog.configure(log.str(), false);
    {
        CaptureCerr cap;
        BOOST_CHECK_EXCEPTION(
            ktlog.fatal("cannot open {}", "x.nodes"), FatalError, [](const FatalError &e) {
                return e.message() == "cannot open x.nodes" && std::string(e.what()) == e.message();
            });
        // The preassembled overload takes the same route.
        BOOST_CHECK_EXCEPTION(ktlog.fatal(std::string_view("literal {brace}")), FatalError,
                              [](const FatalError &e) {
                                  return e.message() == "literal {brace}";
                              });
    }
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(fatal_is_not_a_std_exception) {
    // Library code catches std::exception to skip one bad input record. A fatal
    // must not be mistaken for one of those, so it has to get past such a handler.
    CaptureCerr cap;
    bool swallowed = false;
    bool escaped = false;
    try {
        try {
            ktlog.fatal("must not be swallowed");
        } catch (const std::exception &) {
            swallowed = true;
        }
    } catch (const FatalError &) {
        escaped = true;
    }
    BOOST_TEST(!swallowed);
    BOOST_TEST(escaped);
}

BOOST_AUTO_TEST_CASE(fatal_is_red_and_logged_and_the_log_stays_open) {
    TempLog log;
    ktlog.configure(log.str(), false);

    std::string console;
    {
        CaptureCerr cap;
        BOOST_CHECK_THROW(ktlog.fatal("cannot open {}", "x.nodes"), FatalError);
        console = cap.str();
    }

    BOOST_TEST(console.find(kRed) != std::string::npos);
    BOOST_TEST(console.find("fatal") != std::string::npos);
    BOOST_TEST(console.find("cannot open x.nodes") != std::string::npos);

    const std::string file = log.contents();
    BOOST_TEST(file.find("[fatal]") != std::string::npos);
    BOOST_TEST(file.find("cannot open x.nodes") != std::string::npos);
    BOOST_TEST(file.find('\033') == std::string::npos);

    // fatal() no longer shuts the log down, so a host that caught the error can
    // keep logging into the same transcript.
    const std::size_t before = ktlog.recordCount();
    {
        CaptureCerr cap;
        ktlog.echo("still here after the fatal");
    }
    BOOST_TEST(ktlog.recordCount() == before + 1);
    const std::string after = log.contents();
    BOOST_TEST(after.find("still here after the fatal") != std::string::npos);
    BOOST_TEST(after.find("[fatal]") < after.find("still here after the fatal"));
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
    { const ScopedTimer a("unit-timer-c"); }
    { const ScopedTimer b("unit-timer-d"); }
    TimerRegistry &registry = TimerRegistry::instance();
    const auto snapshot = registry.snapshot();
    BOOST_TEST(snapshot.size() == 2U);
    BOOST_TEST(snapshot[0].first == "unit-timer-c");
    BOOST_TEST(snapshot[1].first == "unit-timer-d");
    BOOST_TEST(registry.totalWallSeconds() >= 0.0);
    BOOST_TEST(registry.totalCpuSeconds() >= 0.0);
    BOOST_TEST(registry.isEnabled("unit-timer-c"));
}

BOOST_AUTO_TEST_CASE(reset_clears_statistics_but_keeps_names_known) {
    TimerRegistry::instance().reset();
    { const ScopedTimer timer("unit-timer-e"); }
    BOOST_TEST(TimerRegistry::instance().find("unit-timer-e") != nullptr);
    TimerRegistry::instance().reset();
    BOOST_TEST(TimerRegistry::instance().find("unit-timer-e") == nullptr);
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
