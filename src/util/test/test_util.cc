#define BOOST_TEST_MODULE ktplace_util
#define BOOST_TEST_DYN_LINK
#include "util/kt_history.h"
#include "util/kt_log.h"
#include "util/kt_platform.h"
#include "util/kt_scopedTimer.h"

#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

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

constexpr const char *yellow = "\033[1;33m";
constexpr const char *red = "\033[1;31m";
constexpr const char *reset = "\033[0m";

}  // namespace

BOOST_AUTO_TEST_CASE(echo_reaches_both_sinks_without_colour) {
    TempLog log;
    ktlog.configure(log.str(), false);

    {
        CaptureCerr cap;
        ktlog.echo("plain record {}", 42);
        BOOST_TEST(cap.str().find("plain record 42") != std::string::npos);
        BOOST_TEST(cap.str().find(yellow) == std::string::npos);
        BOOST_TEST(cap.str().find(red) == std::string::npos);
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

    BOOST_TEST(console.find(yellow) != std::string::npos);
    BOOST_TEST(console.find("warning") != std::string::npos);
    BOOST_TEST(console.find(reset) != std::string::npos);
    BOOST_TEST(console.find("cell 7 is taller than one row") != std::string::npos);

    const std::string file = log.contents();
    BOOST_TEST(file.find("warning: ") != std::string::npos);
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

BOOST_AUTO_TEST_CASE(fatal_is_red_and_exits) {
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
        ::_exit(EXIT_SUCCESS);
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
    BOOST_REQUIRE(WEXITSTATUS(status) != 0);

    BOOST_TEST(console.find(red) != std::string::npos);
    BOOST_TEST(console.find("fatal") != std::string::npos);
    BOOST_TEST(console.find("cannot open x.nodes") != std::string::npos);

    const std::string file = log.contents();
    BOOST_TEST(file.find("fatal: ") != std::string::npos);
    BOOST_TEST(file.find("cannot open x.nodes") != std::string::npos);
    BOOST_TEST(file.find('\033') == std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(a_sink_receives_every_record_with_its_level_and_step) {
    CaptureCerr console;
    std::vector<std::pair<Logger::Level, std::string>> records;
    ktlog.setSink([&records](Logger::Level level, std::string_view line) {
        records.emplace_back(level, std::string(line));
    });
    ktlog.beginStep("sinkstep");
    ktlog.echo("hello {}", 1);
    ktlog.trace("detail");
    ktlog.warning("careful");
    ktlog.setSink(nullptr);
    ktlog.echo("after");

    BOOST_REQUIRE_EQUAL(records.size(), 3U);
    BOOST_TEST((records[0].first == Logger::Level::Echo));
    BOOST_TEST(records[0].second == "[sinkstep] hello 1");
    BOOST_TEST((records[1].first == Logger::Level::Trace));
    BOOST_TEST((records[2].first == Logger::Level::Warning));
    BOOST_TEST(records[2].second == "[sinkstep] warning: careful");
    ktlog.beginStep("ktplace");
}

BOOST_AUTO_TEST_CASE(fatal_throws_instead_of_exiting_when_asked) {
    CaptureCerr console;
    ktlog.throwOnFatal(true);
    std::string thrown;
    try {
        ktlog.fatal("cannot open {}", "y.nodes");
    } catch (const FatalError &e) {
        thrown = e.what();
    }
    ktlog.throwOnFatal(false);
    BOOST_TEST(thrown == "cannot open y.nodes");
    BOOST_TEST(console.str().find("cannot open y.nodes") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_trace_log_holds_every_level_and_the_log_all_but_trace) {
    TempLog log;
    ktlog.configure(log.str(), false);
    {
        CaptureCerr cap;
        ktlog.echo("an echo");
        ktlog.warning("a warning");
        ktlog.trace("a trace");
    }
    ktlog.configure("", false);
    std::ifstream in(Logger::tracePathFor(log.str()));
    std::ostringstream trace;
    trace << in.rdbuf();
    std::filesystem::remove(Logger::tracePathFor(log.str()));

    BOOST_TEST(log.contents() == "[ktplace] an echo\n[ktplace] warning: a warning\n");
    BOOST_TEST(trace.str() ==
               "[ktplace] an echo\n[ktplace] warning: a warning\n[ktplace] a trace\n");
}

BOOST_AUTO_TEST_CASE(a_step_prints_a_centred_banner_and_prefixes_its_lines) {
    TempLog log;
    ktlog.configure(log.str(), false);
    {
        CaptureCerr cap;
        ktlog.beginStep("load");
        ktlog.echo("reading");
    }
    ktlog.configure("", false);
    ktlog.beginStep("ktplace");
    std::filesystem::remove(Logger::tracePathFor(log.str()));

    const std::string rule(60, '#');
    const std::string title = '#' + std::string(27, ' ') + "load" + std::string(27, ' ') + '#';
    BOOST_TEST(log.contents() == rule + "\n" + title + "\n" + rule + "\n[load] reading\n");
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

BOOST_AUTO_TEST_CASE(command_history_persists_across_sessions) {
    const std::filesystem::path file =
        std::filesystem::temp_directory_path() / ("ktplace_history_" + std::to_string(getpid()));
    std::filesystem::remove(file);
    {
        CommandHistory history(file.string());
        history.add("read_design ibm01");
        history.add("gplace_design");
        history.add("gplace_design");
        history.add("");
        history.add("foreach s {1 2} {\n}");
        BOOST_TEST(history.entries().size() == 2U);
    }
    const CommandHistory reopened(file.string());
    BOOST_TEST(
        reopened.entries() == (std::vector<std::string>{"read_design ibm01", "gplace_design"}),
        boost::test_tools::per_element());
    std::filesystem::remove(file);
}

BOOST_AUTO_TEST_CASE(command_history_keeps_only_the_newest_entries) {
    const std::filesystem::path file = std::filesystem::temp_directory_path() /
                                       ("ktplace_history_cap_" + std::to_string(getpid()));
    {
        std::ofstream out(file);
        for (std::size_t i = 0; i < CommandHistory::capacity + 5; ++i) {
            out << "cmd" << i << '\n';
        }
    }
    const CommandHistory history(file.string());
    BOOST_TEST(history.entries().size() == CommandHistory::capacity);
    BOOST_TEST(history.entries().front() == "cmd5");
    const CommandHistory reread(file.string());
    BOOST_TEST(reread.entries().size() == CommandHistory::capacity);
    std::filesystem::remove(file);
}

BOOST_AUTO_TEST_CASE(command_history_without_a_file_lives_in_memory) {
    CommandHistory history("");
    history.add("help");
    BOOST_TEST(history.entries().size() == 1U);
    BOOST_TEST(history.file().empty());
}

BOOST_AUTO_TEST_CASE(the_platform_reports_its_cpu_threads_and_memory) {
    const PlatformInfo info = PlatformInfo::detect();
    BOOST_TEST(!info.os.empty());
    BOOST_TEST(!info.cpu.empty());
    BOOST_TEST(!info.compiler.empty());
    BOOST_TEST(info.threads > 0U);
    BOOST_TEST(info.cores > 0U);
    BOOST_TEST(info.cores <= info.threads);
    BOOST_TEST(info.memoryBytes > 0U);
    BOOST_TEST(info.availableBytes <= info.memoryBytes);
}
