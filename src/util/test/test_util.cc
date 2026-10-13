#define BOOST_TEST_MODULE ktplace_util
#define BOOST_TEST_DYN_LINK
#include "detailPlacer/fastdp/fastdp_placer.h"
#include "legalizer/abacus/abacus_legalizer.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/simpl_placer.h"
#include "util/kt_log.h"
#include "util/kt_param.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_plotOptions.h"

#include <atomic>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tbb/parallel_for.h>
#include <thread>
#include <utility>
#include <variant>
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

// ---------------------------------------------------------------------------
// Parameter registry

namespace {

// Sets or clears one environment variable for the scope of a test, restoring
// what was there before, and drops any explicit registry value on both ends.
class EnvGuard {
public:
    explicit EnvGuard(const char *name) : name_(name) {
        if (const char *old = std::getenv(name)) {
            old_ = old;
        }
        ParamRegistry::instance().resetAll();
    }
    ~EnvGuard() {
        if (old_) {
            ::setenv(name_, old_->c_str(), 1);
        } else {
            ::unsetenv(name_);
        }
        ParamRegistry::instance().resetAll();
    }
    EnvGuard(const EnvGuard &) = delete;
    EnvGuard &operator=(const EnvGuard &) = delete;

    void set(const char *value) const {
        ::setenv(name_, value, 1);
    }
    void clear() const {
        ::unsetenv(name_);
    }

private:
    const char *name_;
    std::optional<std::string> old_;
};

ParamSource sourceOf(std::string_view name) {
    return ParamRegistry::instance().state(name).value().source;
}

}  // namespace

BOOST_AUTO_TEST_CASE(param_precedence_is_default_then_environment_then_set) {
    auto &reg = ParamRegistry::instance();
    const EnvGuard env("KTPLACE_SIMPL_ITERS");
    env.clear();
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_ITERS") == 100);
    BOOST_TEST(!reg.overridden<std::int64_t>("KTPLACE_SIMPL_ITERS").has_value());
    BOOST_TEST((sourceOf("KTPLACE_SIMPL_ITERS") == ParamSource::Default));

    env.set("25");
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_ITERS") == 25);
    BOOST_TEST((sourceOf("KTPLACE_SIMPL_ITERS") == ParamSource::Environment));

    BOOST_TEST(reg.set("KTPLACE_SIMPL_ITERS", "7").has_value());
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_ITERS") == 7);
    BOOST_TEST(*reg.overridden<std::int64_t>("KTPLACE_SIMPL_ITERS") == 7);
    BOOST_TEST((sourceOf("KTPLACE_SIMPL_ITERS") == ParamSource::Explicit));
    BOOST_TEST(reg.state("KTPLACE_SIMPL_ITERS")->text == "7");

    BOOST_TEST(reg.reset("KTPLACE_SIMPL_ITERS").has_value());
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_ITERS") == 25);
    env.clear();
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_ITERS") == 100);
}

BOOST_AUTO_TEST_CASE(param_environment_keeps_each_sites_old_parsing) {
    auto &reg = ParamRegistry::instance();
    {
        // std::atoi semantics: the leading number, garbage is 0, "true" is 0.
        const EnvGuard env("KTPLACE_SIMPL_FENCES");
        env.set("true");
        BOOST_TEST(!reg.get<bool>("KTPLACE_SIMPL_FENCES"));
        env.set("2");
        BOOST_TEST(reg.get<bool>("KTPLACE_SIMPL_FENCES"));
        env.set("0");
        BOOST_TEST(!reg.get<bool>("KTPLACE_SIMPL_FENCES"));
    }
    {
        const EnvGuard env("KTPLACE_NTU_LEAF_CELLS");
        env.set("12abc");
        BOOST_TEST(*reg.overridden<std::size_t>("KTPLACE_NTU_LEAF_CELLS") == 12u);
        env.set("abc");
        BOOST_TEST(*reg.overridden<std::size_t>("KTPLACE_NTU_LEAF_CELLS") == 0u);
    }
    {
        const EnvGuard env("KTPLACE_SIMPL_DENSITY");
        env.set("0.85x");
        BOOST_TEST(*reg.overridden<double>("KTPLACE_SIMPL_DENSITY") == 0.85);
    }
    {
        // std::stod semantics: garbage keeps the default, any non-zero number is on.
        const EnvGuard env("KTPLACE_ANIM");
        env.set("garbage");
        BOOST_TEST(!reg.overridden<bool>("KTPLACE_ANIM").has_value());
        BOOST_TEST(reg.get<bool>("KTPLACE_ANIM"));
        BOOST_TEST((sourceOf("KTPLACE_ANIM") == ParamSource::Default));
        env.set("0.0");
        BOOST_TEST(!reg.get<bool>("KTPLACE_ANIM"));
        BOOST_TEST(!PlotOptions::fromEnvironment().animate);
    }
    {
        const EnvGuard env("KTPLACE_ANIM_MAX_FRAMES");
        env.set("1e3");
        BOOST_TEST(reg.get<std::size_t>("KTPLACE_ANIM_MAX_FRAMES") == 1000u);
        BOOST_TEST(PlotOptions::fromEnvironment().frameBudget == 1000u);
    }
    {
        const EnvGuard env("KTPLACE_ANIM_DELAY_CS");
        env.set("7.9");
        BOOST_TEST(PlotOptions::fromEnvironment().frameDelayCs == 7);
    }
    {
        // Presence: "0" still means "set".
        const EnvGuard env("KTPLACE_DP_NO_MEDIAN");
        env.clear();
        BOOST_TEST(!reg.get<bool>("KTPLACE_DP_NO_MEDIAN"));
        env.set("0");
        BOOST_TEST(reg.get<bool>("KTPLACE_DP_NO_MEDIAN"));
    }
    {
        // Text: passed through untouched for the site to judge.
        const EnvGuard env("KTPLACE_SIMPL_STRIPE_SCALE");
        env.set("sideways");
        BOOST_TEST(*reg.overridden<std::string>("KTPLACE_SIMPL_STRIPE_SCALE") == "sideways");
    }
}

BOOST_AUTO_TEST_CASE(param_set_parses_strictly_by_type) {
    auto &reg = ParamRegistry::instance();
    const EnvGuard a("KTPLACE_FINAL_PPM");
    const EnvGuard b("KTPLACE_SIMPL_SEED");
    const EnvGuard c("KTPLACE_SIMPL_CG_TOL");
    const EnvGuard d("KTPLACE_SIMPL_START");
    const EnvGuard e("KTPLACE_SIMPL_GRID");

    for (const char *on : {"1", "true", "TRUE", "on", "yes", " true "}) {
        BOOST_TEST(reg.set("KTPLACE_FINAL_PPM", on).has_value());
        BOOST_TEST(reg.get<bool>("KTPLACE_FINAL_PPM"));
    }
    for (const char *off : {"0", "false", "off", "no"}) {
        BOOST_TEST(reg.set("KTPLACE_FINAL_PPM", off).has_value());
        BOOST_TEST(!reg.get<bool>("KTPLACE_FINAL_PPM"));
    }
    BOOST_TEST((reg.set("KTPLACE_FINAL_PPM", "2").error().kind == ParamError::Kind::Malformed));

    BOOST_TEST(reg.set("KTPLACE_SIMPL_SEED", "42").has_value());
    BOOST_TEST(reg.get<std::size_t>("KTPLACE_SIMPL_SEED") == 42u);
    BOOST_TEST((reg.set("KTPLACE_SIMPL_SEED", "-1").error().kind == ParamError::Kind::Malformed));
    BOOST_TEST(
        (reg.set("KTPLACE_SIMPL_SEED", "12abc").error().kind == ParamError::Kind::Malformed));

    BOOST_TEST(reg.set("KTPLACE_SIMPL_CG_TOL", "1e-4").has_value());
    BOOST_TEST(reg.get<double>("KTPLACE_SIMPL_CG_TOL") == 1e-4);
    BOOST_TEST(
        (reg.set("KTPLACE_SIMPL_CG_TOL", "nan").error().kind == ParamError::Kind::Malformed));
    BOOST_TEST((reg.set("KTPLACE_SIMPL_CG_TOL", "0").error().kind == ParamError::Kind::OutOfRange));

    BOOST_TEST(reg.set("KTPLACE_SIMPL_START", "input").has_value());
    BOOST_TEST(reg.get<std::string>("KTPLACE_SIMPL_START") == "input");

    // 0 is "automatic", allowed beside the 8..1024 range.
    BOOST_TEST(reg.set("KTPLACE_SIMPL_GRID", "0").has_value());
    BOOST_TEST(reg.set("KTPLACE_SIMPL_GRID", "64").has_value());
    BOOST_TEST(reg.get<std::int64_t>("KTPLACE_SIMPL_GRID") == 64);
    BOOST_TEST((reg.set("KTPLACE_SIMPL_GRID", "5").error().kind == ParamError::Kind::OutOfRange));
}

BOOST_AUTO_TEST_CASE(param_invalid_set_is_rejected_and_keeps_the_old_value) {
    auto &reg = ParamRegistry::instance();
    const EnvGuard w("KTPLACE_DP_WINDOW");
    const EnvGuard s("KTPLACE_SIMPL_STRIPE_SCALE");

    BOOST_TEST(reg.set("KTPLACE_DP_WINDOW", "8").has_value());
    const auto range = reg.set("KTPLACE_DP_WINDOW", "20");
    BOOST_REQUIRE(!range.has_value());
    BOOST_TEST((range.error().kind == ParamError::Kind::OutOfRange));
    BOOST_TEST(range.error().message.find("KTPLACE_DP_WINDOW") != std::string::npos);
    BOOST_TEST(reg.get<std::size_t>("KTPLACE_DP_WINDOW") == 8u);
    BOOST_TEST(!reg.set("KTPLACE_DP_WINDOW", "abc").has_value());
    BOOST_TEST(reg.get<std::size_t>("KTPLACE_DP_WINDOW") == 8u);

    // A rejected first set leaves the parameter at its environment/default.
    s.set("tight");
    const auto choice = reg.set("KTPLACE_SIMPL_STRIPE_SCALE", "sideways");
    BOOST_REQUIRE(!choice.has_value());
    BOOST_TEST((choice.error().kind == ParamError::Kind::NotAChoice));
    BOOST_TEST(reg.get<std::string>("KTPLACE_SIMPL_STRIPE_SCALE") == "tight");
    BOOST_TEST((sourceOf("KTPLACE_SIMPL_STRIPE_SCALE") == ParamSource::Environment));
}

BOOST_AUTO_TEST_CASE(param_unknown_names_are_reported_not_fatal) {
    auto &reg = ParamRegistry::instance();
    BOOST_TEST(reg.find("KTPLACE_NO_SUCH_THING") == nullptr);
    const auto found = reg.lookup("KTPLACE_NO_SUCH_THING");
    BOOST_REQUIRE(!found.has_value());
    BOOST_TEST((found.error().kind == ParamError::Kind::UnknownName));
    BOOST_TEST(found.error().message.find("KTPLACE_NO_SUCH_THING") != std::string::npos);
    BOOST_TEST(
        (reg.set("KTPLACE_NO_SUCH_THING", "1").error().kind == ParamError::Kind::UnknownName));
    BOOST_TEST(!reg.reset("KTPLACE_NO_SUCH_THING").has_value());
    BOOST_TEST(!reg.state("KTPLACE_NO_SUCH_THING").has_value());
    BOOST_TEST(reg.lookup("KTPLACE_ANIM").value()->name == "KTPLACE_ANIM");

    // Reading one from code is a programming error, and says so.
    BOOST_CHECK_THROW((void)reg.get<bool>("KTPLACE_NO_SUCH_THING"), std::logic_error);
    BOOST_CHECK_THROW((void)reg.get<double>("KTPLACE_ANIM"), std::logic_error);
}

BOOST_AUTO_TEST_CASE(param_listing_holds_every_declared_parameter) {
    // Every variable the engine reads. A new one is declared in kt_param.cc and
    // added here, so the list cannot drift from the code without a test noticing.
    const std::vector<std::string_view> expected = {
        "KTPLACE_ANIM",
        "KTPLACE_ANIM_MAX_FRAMES",
        "KTPLACE_ANIM_MAX_BYTES",
        "KTPLACE_ANIM_DELAY_CS",
        "KTPLACE_ANIM_BLEND",
        "KTPLACE_ANIM_ZOOM",
        "KTPLACE_FINAL_ZOOM",
        "KTPLACE_FINAL_PPM",
        "KTPLACE_ANIM_FINAL_HOLD",
        "KTPLACE_ANIM_TRACE_EVERY",
        "KTPLACE_ANIM_CG_EVERY",
        "KTPLACE_ANIM_DENSITY_MAPS",
        "KTPLACE_SIMPL_FENCES",
        "KTPLACE_SIMPL_GRID",
        "KTPLACE_SIMPL_CG_TOL",
        "KTPLACE_SIMPL_STRIPE_SCALE",
        "KTPLACE_SIMPL_INIT_NET",
        "KTPLACE_SIMPL_LSS_NET",
        "KTPLACE_SIMPL_INIT_ITERS",
        "KTPLACE_SIMPL_ITERS",
        "KTPLACE_SIMPL_SEED",
        "KTPLACE_SIMPL_DENSITY",
        "KTPLACE_SIMPL_GLOBAL_CLUSTER",
        "KTPLACE_SIMPL_PSEUDONET",
        "KTPLACE_SIMPL_START",
        "KTPLACE_SIMPL_ALPHA_BASE",
        "KTPLACE_SIMPL_ALPHA_DECAY",
        "KTPLACE_SIMPL_ALPHA_DECAY_BELOW",
        "KTPLACE_SIMPL_NO_LAL",
        "KTPLACE_SIMPL_LAL_ONLY",
        "KTPLACE_NTU_LEAF_CELLS",
        "KTPLACE_NTU_MAX_LEVELS",
        "KTPLACE_NTU_RETRIES",
        "KTPLACE_NTU_VERBOSE",
        "KTPLACE_ABACUS_MAX_ROW_DIST",
        "KTPLACE_ABACUS_MIN_SUBROW_SITES",
        "KTPLACE_ABACUS_CLUSTER_GAP",
        "KTPLACE_DP_WINDOW",
        "KTPLACE_DP_NO_MEDIAN",
    };
    auto &reg = ParamRegistry::instance();
    reg.resetAll();
    const std::vector<ParamState> listed = reg.list();
    BOOST_TEST(listed.size() == reg.definitions().size());
    BOOST_TEST(listed.size() == expected.size());
    std::set<std::string_view> names;
    for (const ParamState &st : listed) {
        BOOST_REQUIRE(st.def != nullptr);
        names.insert(st.def->name);
        BOOST_TEST(!st.def->description.empty());
        BOOST_TEST(!st.def->group.empty());
        BOOST_TEST(st.text == ParamRegistry::format(st.value));
        // Every default is itself a value set() accepts.
        BOOST_TEST(reg.set(st.def->name, ParamRegistry::format(st.def->defaultValue)).has_value(),
                   st.def->name);
    }
    reg.resetAll();
    BOOST_TEST(names.size() == listed.size());
    for (const std::string_view name : expected) {
        BOOST_TEST(names.contains(name), name);
    }
}

BOOST_AUTO_TEST_CASE(param_defaults_match_the_stage_params_structs) {
    const auto &reg = ParamRegistry::instance();
    // The declared defaults, so the environment of the test run is irrelevant.
    const auto def = [&reg](std::string_view name) -> const ParamValue & {
        return reg.find(name)->defaultValue;
    };
    const PlotOptions plot;
    BOOST_TEST(std::get<bool>(def("KTPLACE_ANIM")) == plot.animate);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ANIM_MAX_FRAMES")) == plot.frameBudget);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ANIM_MAX_BYTES")) == plot.gifByteCap);
    BOOST_TEST(std::get<std::int64_t>(def("KTPLACE_ANIM_DELAY_CS")) == plot.frameDelayCs);
    BOOST_TEST(std::get<std::int64_t>(def("KTPLACE_ANIM_BLEND")) == plot.blendFrames);
    BOOST_TEST(std::get<double>(def("KTPLACE_ANIM_ZOOM")) == plot.frameZoom);
    BOOST_TEST(std::get<double>(def("KTPLACE_FINAL_ZOOM")) == plot.finalZoom);
    BOOST_TEST(std::get<bool>(def("KTPLACE_FINAL_PPM")) == plot.writePpm);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ANIM_FINAL_HOLD")) == plot.finalHold);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ANIM_TRACE_EVERY")) == plot.traceEvery);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ANIM_CG_EVERY")) == plot.cgFrameEvery);
    BOOST_TEST(std::get<bool>(def("KTPLACE_ANIM_DENSITY_MAPS")) == plot.densityMaps);

    using simpl::SimplParams;
    const SimplParams sp;
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_CG_TOL")) == sp.cgTol);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_SIMPL_INIT_ITERS")) == sp.initMaxIters);
    BOOST_TEST(std::get<std::int64_t>(def("KTPLACE_SIMPL_ITERS")) ==
               static_cast<std::int64_t>(sp.maxIters));
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_SIMPL_SEED")) == sp.seed);
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_DENSITY")) == sp.densityLimit);
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_GLOBAL_CLUSTER")) == sp.globalClusterFrac);
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_ALPHA_BASE")) == sp.alphaBase);
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_ALPHA_DECAY")) == sp.alphaDecay);
    BOOST_TEST(std::get<double>(def("KTPLACE_SIMPL_ALPHA_DECAY_BELOW")) == sp.alphaDecayBelow);
    BOOST_TEST(std::get<bool>(def("KTPLACE_SIMPL_NO_LAL")) == !sp.lookAhead);
    BOOST_TEST((sp.stripeScaleMode == SimplParams::StripeScale::Both));
    BOOST_TEST(std::get<std::string>(def("KTPLACE_SIMPL_STRIPE_SCALE")) == "both");
    BOOST_TEST((sp.initNetModel == SimplParams::NetModel::Star));
    BOOST_TEST(std::get<std::string>(def("KTPLACE_SIMPL_INIT_NET")) == "star");
    BOOST_TEST((sp.lssNetModel == SimplParams::NetModel::B2B));
    BOOST_TEST(std::get<std::string>(def("KTPLACE_SIMPL_LSS_NET")) == "b2b");
    BOOST_TEST((sp.pseudonetLaw == SimplParams::PseudonetLaw::ConstantStiffness));
    BOOST_TEST(std::get<std::string>(def("KTPLACE_SIMPL_PSEUDONET")) == "const");
    BOOST_TEST((sp.start == SimplParams::StartPlacement::Uniform));
    BOOST_TEST(std::get<std::string>(def("KTPLACE_SIMPL_START")) == "uniform");

    const RatioPlaceParams ntu;
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_NTU_LEAF_CELLS")) == ntu.targetLeafCells);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_NTU_MAX_LEVELS")) == ntu.maxLevels);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_NTU_RETRIES")) == ntu.maxRatioRetries);
    BOOST_TEST(std::get<bool>(def("KTPLACE_NTU_VERBOSE")) == ntu.verbose);

    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_ABACUS_MAX_ROW_DIST")) ==
               LegalizeParams{}.maxRowDistance);
    BOOST_TEST(std::get<std::size_t>(def("KTPLACE_DP_WINDOW")) ==
               DetailPlaceParams{}.localReorderWindow);
}

BOOST_AUTO_TEST_CASE(param_reads_are_safe_from_many_threads) {
    auto &reg = ParamRegistry::instance();
    const EnvGuard env("KTPLACE_SIMPL_ALPHA_BASE");
    env.set("0.5");
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bad{0};
    std::atomic<std::size_t> reads{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 8; ++t) {
        readers.emplace_back([&] {
            while (!stop.load(std::memory_order_relaxed)) {
                const double v = reg.get<double>("KTPLACE_SIMPL_ALPHA_BASE");
                if (v != 0.5 && v != 0.25) {
                    bad.fetch_add(1);
                }
                (void)reg.list();
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // The writer flips between an explicit value and the environment's.
    for (int i = 0; i < 2000; ++i) {
        BOOST_REQUIRE(reg.set("KTPLACE_SIMPL_ALPHA_BASE", "0.25").has_value());
        BOOST_REQUIRE(reg.reset("KTPLACE_SIMPL_ALPHA_BASE").has_value());
    }
    stop = true;
    for (std::thread &t : readers) {
        t.join();
    }
    BOOST_TEST(bad.load() == 0u);
    BOOST_TEST(reads.load() > 0u);
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

BOOST_AUTO_TEST_CASE(a_sink_throwing_an_empty_message_is_still_reported) {
    TempLog log;
    ktlog.configure(log.str(), false);
    {
        CaptureCerr cap;
        const SinkGuard silent([](LogLevel, std::string_view) {
            throw std::runtime_error("");
        });
        ktlog.echo("record");
    }
    BOOST_TEST(log.contents().find("threw (no message)") != std::string::npos);
    ktlog.configure("", false);
}

BOOST_AUTO_TEST_CASE(a_sink_may_add_another_sink) {
    CaptureCerr cap;
    Collected late;
    Logger::SinkId added = 0;
    {
        const SinkGuard adder([&](LogLevel, std::string_view) {
            if (added == 0) {
                added = ktlog.addSink(late.sink());
            }
        });
        ktlog.echo("one");  // the new sink is not in this record's snapshot
        ktlog.echo("two");
    }
    ktlog.removeSink(added);
    BOOST_TEST(late.size() == 1U);
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
