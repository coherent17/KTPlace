#define BOOST_TEST_MODULE ktplace_util
#define BOOST_TEST_DYN_LINK
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tbb/parallel_for.h>
#include <thread>
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

namespace {

/// Removes a sink at scope exit, so a failing check cannot leave it registered
/// for the tests that follow.
class SinkGuard {
public:
    explicit SinkGuard(Logger::Sink sink) : id(ktlog.addSink(std::move(sink))) {}
    ~SinkGuard() {
        ktlog.removeSink(id);
    }
    SinkGuard(const SinkGuard &) = delete;
    SinkGuard &operator=(const SinkGuard &) = delete;

    Logger::SinkId id;
};

/// A thread-safe record of what a sink saw.
struct Collected {
    std::mutex mutex;
    std::vector<std::pair<LogLevel, std::string>> records;

    Logger::Sink sink() {
        return [this](LogLevel level, std::string_view text) {
            const std::lock_guard<std::mutex> lock(mutex);
            records.emplace_back(level, std::string(text));
        };
    }
    std::size_t size() {
        const std::lock_guard<std::mutex> lock(mutex);
        return records.size();
    }
};

}  // namespace

BOOST_AUTO_TEST_CASE(a_sink_receives_every_record_with_its_level) {
    TempLog log;
    ktlog.configure(log.str(), false);  // not verbose: trace stays off stderr...
    Collected seen;
    {
        const SinkGuard guard(seen.sink());
        CaptureCerr cap;
        ktlog.echo("e {}", 1);
        ktlog.trace("t {}", 2);  // ...but a sink still gets it, to filter itself
        ktlog.warning("w {}", 3);
        BOOST_CHECK_THROW(ktlog.fatal("f {}", 4), FatalError);
    }
    BOOST_REQUIRE_EQUAL(seen.records.size(), 4U);
    BOOST_TEST((seen.records[0] == std::pair{LogLevel::Echo, std::string("e 1")}));
    BOOST_TEST((seen.records[1] == std::pair{LogLevel::Trace, std::string("t 2")}));
    BOOST_TEST((seen.records[2] == std::pair{LogLevel::Warning, std::string("w 3")}));
    BOOST_TEST((seen.records[3] == std::pair{LogLevel::Fatal, std::string("f 4")}));
    BOOST_TEST(std::string(Logger::levelTag(LogLevel::Warning)) == "warning");
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(a_removed_sink_stops_receiving) {
    CaptureCerr cap;
    Collected kept;
    Collected dropped;
    const SinkGuard keep(kept.sink());
    const Logger::SinkId id = ktlog.addSink(dropped.sink());
    ktlog.echo("before");
    ktlog.removeSink(id);
    ktlog.removeSink(id);       // twice is harmless
    ktlog.removeSink(9999999);  // so is an id never handed out
    ktlog.echo("after");
    BOOST_TEST(dropped.size() == 1U);
    BOOST_TEST(kept.size() == 2U);
}

BOOST_AUTO_TEST_CASE(concurrent_records_all_reach_a_sink) {
    constexpr int kThreads = 8;
    constexpr int kPerThread = 500;
    CaptureCerr cap;
    Collected seen;
    {
        const SinkGuard guard(seen.sink());
        std::vector<std::thread> workers;
        for (int t = 0; t < kThreads; ++t) {
            workers.emplace_back([t] {
                for (int i = 0; i < kPerThread; ++i) {
                    ktlog.echo("t{} r{}", t, i);
                }
            });
        }
        for (std::thread &w : workers) {
            w.join();
        }
    }
    BOOST_REQUIRE_EQUAL(seen.records.size(), static_cast<std::size_t>(kThreads * kPerThread));
    std::set<std::string> distinct;
    for (const auto &[level, text] : seen.records) {
        BOOST_TEST((level == LogLevel::Echo));
        distinct.insert(text);
    }
    BOOST_TEST(distinct.size() == static_cast<std::size_t>(kThreads * kPerThread));
}

BOOST_AUTO_TEST_CASE(a_throwing_sink_is_contained) {
    TempLog log;
    ktlog.configure(log.str(), false);
    Collected after;
    {
        CaptureCerr cap;
        const SinkGuard bad([](LogLevel, std::string_view) {
            throw std::runtime_error("boom");
        });
        const SinkGuard worse([](LogLevel, std::string_view) {
            throw 42;
        });
        const SinkGuard good(after.sink());
        BOOST_CHECK_NO_THROW(ktlog.echo("first"));
        BOOST_CHECK_NO_THROW(ktlog.echo("second"));
        // A fatal still throws its own FatalError, not the sink's exception.
        BOOST_CHECK_THROW(ktlog.fatal("third"), FatalError);
    }
    // The sink after the throwing ones still got every record.
    BOOST_TEST(after.size() == 3U);
    const std::string file = log.contents();
    BOOST_TEST(file.find("first") != std::string::npos);
    BOOST_TEST(file.find("second") != std::string::npos);
    BOOST_TEST(file.find("[fatal] third") != std::string::npos);
    // Each throwing sink is reported once, not once per record.
    std::size_t reports = 0;
    for (std::size_t at = file.find("threw"); at != std::string::npos;
         at = file.find("threw", at + 1)) {
        ++reports;
    }
    BOOST_TEST(reports == 2U);
    BOOST_TEST(file.find("boom") != std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(a_sink_may_log_and_remove_itself_without_deadlock) {
    TempLog log;
    ktlog.configure(log.str(), false);
    CaptureCerr cap;
    std::atomic<int> calls{0};
    Logger::SinkId self = 0;
    self = ktlog.addSink([&](LogLevel, std::string_view text) {
        ++calls;
        // Reaches the transcript, but is not fed back into the sinks.
        ktlog.echo("sink saw: {}", text);
        ktlog.removeSink(self);
    });
    ktlog.echo("trigger");
    ktlog.echo("not delivered");
    BOOST_TEST(calls.load() == 1);
    BOOST_TEST(log.contents().find("sink saw: trigger") != std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(remove_sink_waits_for_an_in_flight_call) {
    CaptureCerr cap;
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> removed{false};
    const Logger::SinkId id = ktlog.addSink([&](LogLevel, std::string_view) {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    std::thread emitter([] {
        ktlog.echo("held");
    });
    while (!entered) {
        std::this_thread::yield();
    }
    std::thread remover([&] {
        ktlog.removeSink(id);
        removed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOST_TEST(!removed.load());  // the callback is still running
    release = true;
    remover.join();
    emitter.join();
    BOOST_TEST(removed.load());
}

BOOST_AUTO_TEST_CASE(a_fatal_inside_a_tbb_region_reaches_the_caller) {
    CaptureCerr cap;
    BOOST_CHECK_EXCEPTION(tbb::parallel_for(0, 64,
                                            [](int i) {
                                                if (i == 17) {
                                                    ktlog.fatal("worker {} failed", i);
                                                }
                                            }),
                          FatalError, [](const FatalError &e) {
                              return e.message() == "worker 17 failed";
                          });
}
