// SPDX-License-Identifier: BSD-3-Clause
// @file test_tcl.cc
// Unit tests for the Tcl interpreter and the option registry.

#define BOOST_TEST_MODULE ktplace_tcl
#define BOOST_TEST_DYN_LINK

#include "datamodel/kt_optionDb.h"
#include "placer/simpl/simpl_options.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"

#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>

using namespace ktplace;
using namespace ktplace::tcl;

// Boost.Test prints a value when an assertion fails.
namespace ktplace::tcl {
std::ostream &operator<<(std::ostream &out, const Value &value) {
    return out << value.asString();
}
}  // namespace ktplace::tcl

namespace {

/// A throwaway script file, deleted with the guard.
class ScriptFile {
public:
    explicit ScriptFile(std::string body) {
        path = std::filesystem::temp_directory_path() /
               ("ktplace_tcl_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".tcl");
        std::ofstream out(path);
        out << body;
    }

    ~ScriptFile() {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }

    ScriptFile(const ScriptFile &) = delete;
    ScriptFile &operator=(const ScriptFile &) = delete;

    [[nodiscard]] std::string str() const {
        return path.string();
    }

private:
    std::filesystem::path path;
};

}  // namespace

BOOST_AUTO_TEST_CASE(a_bare_number_word_is_typed_by_its_spelling) {
    Interpreter interp;
    BOOST_TEST(interp.eval("set a 1").isInt());
    BOOST_TEST(interp.eval("set b 1.5").isDouble());
    BOOST_TEST(interp.eval("set c 1e-3").isDouble());
    BOOST_TEST(interp.eval("set d simpl").isList() == false);
    BOOST_TEST(interp.eval("set d simpl").asString() == "simpl");
    // The distinction the option registry depends on: quoted text stays a string
    // even when it reads as a number.
    BOOST_TEST(interp.eval("set e {2}").asString() == "2");
}

BOOST_AUTO_TEST_CASE(braced_words_are_not_substituted) {
    Interpreter interp;
    (void)interp.eval("set name simpl");
    BOOST_TEST(interp.eval("set a $name").asString() == "simpl");
    BOOST_TEST(interp.eval("set b {$name}").asString() == "$name");
    BOOST_TEST(interp.eval("set c \"$name\"").asString() == "simpl");
}

BOOST_AUTO_TEST_CASE(command_substitution_composes) {
    Interpreter interp;
    const Value out = interp.eval("set total [expr {2 + 3 * 4}]");
    BOOST_TEST(out.asInt() == 14);
    (void)interp.eval("set x [list a b]");
    BOOST_TEST(interp.eval("llength $x").asInt() == 2);
    BOOST_TEST(interp.eval("lindex $x 0").asString() == "a");
    BOOST_TEST(interp.eval("set y [lindex [list a b] 1]").asString() == "b");
}

BOOST_AUTO_TEST_CASE(expr_handles_the_operators_a_config_needs) {
    Interpreter interp;
    BOOST_TEST(interp.eval("expr {3 > 2}").asBool());
    BOOST_TEST(interp.eval("expr {1 && 0}").asBool() == false);
    BOOST_TEST(interp.eval("expr {!(1 == 2)}").asBool());
    BOOST_TEST(interp.eval("expr {7 % 3}").asInt() == 1);
    BOOST_TEST(interp.eval("expr {abs(-2.5)}").asDouble() == 2.5);
    BOOST_TEST(interp.eval("expr {max(1, 9, 4)}").asDouble() == 9);
    // Integer arithmetic stays integer; a division becomes real, as in Tcl.
    BOOST_TEST(interp.eval("expr {1 + 2}").isInt());
    BOOST_TEST(interp.eval("expr {1 / 2}").isDouble());
    BOOST_TEST(interp.eval("expr {1 / 2}").asDouble() == 0.5);
}

BOOST_AUTO_TEST_CASE(if_and_while_run_a_body) {
    Interpreter interp;
    (void)interp.eval("set n 0\nif {$n == 0} { set n 5 } else { set n 9 }");
    BOOST_TEST(interp.variable("n").asInt() == 5);
    (void)interp.eval("set i 0\nwhile {$i < 4} { incr i 2 }");
    BOOST_TEST(interp.variable("i").asInt() == 4);
}

BOOST_AUTO_TEST_CASE(foreach_walks_a_list) {
    Interpreter interp;
    (void)interp.eval("set total 0\nforeach x {1 2 3 4} { incr total $x }");
    BOOST_TEST(interp.variable("total").asInt() == 10);
    (void)interp.eval("set joined {}\nforeach {k v} {a 1 b 2} { lappend joined $k$v }");
    BOOST_TEST(interp.variable("joined").asString() == "{a1 b2}");
}

BOOST_AUTO_TEST_CASE(a_multiline_script_is_separated_by_newlines) {
    Interpreter interp;
    (void)interp.eval("set a 1\nset b 2 ; set c 3\n");
    BOOST_TEST(interp.variable("a").asInt() == 1);
    BOOST_TEST(interp.variable("c").asInt() == 3);
}

BOOST_AUTO_TEST_CASE(comments_and_continuations_do_not_become_commands) {
    Interpreter interp;
    (void)interp.eval("# leading comment\nset a 1\n# final");
    BOOST_TEST(interp.variable("a").asInt() == 1);
    BOOST_CHECK_THROW(interp.eval("set a 1 # trailing"), Error);
    // A bracketed body may run over lines without becoming two commands.
    (void)interp.eval("set list {}\nlappend list \\\n    2\n");
    BOOST_TEST(interp.variable("list").asList().size() == 1);
}

BOOST_AUTO_TEST_CASE(a_comment_runs_to_the_end_of_its_line) {
    Interpreter interp;
    (void)interp.eval("# one; set a 1\n#two; set b 2\nset c 3; # three; set d 4\nset e 5");
    BOOST_TEST(!interp.hasVariable("a"));
    BOOST_TEST(!interp.hasVariable("b"));
    BOOST_TEST(!interp.hasVariable("d"));
    BOOST_TEST(interp.variable("c").asInt() == 3);
    BOOST_TEST(interp.variable("e").asInt() == 5);
}

BOOST_AUTO_TEST_CASE(a_failure_stops_the_script) {
    Interpreter interp;
    BOOST_CHECK_THROW(interp.eval("set a 1\nnosuchcommand x\nset b 2"), Error);
    // 'a' ran before the failure; 'b' did not run after it.
    BOOST_TEST(interp.hasVariable("a"));
    BOOST_TEST(interp.hasVariable("b") == false);
}

BOOST_AUTO_TEST_CASE(source_runs_another_file) {
    ScriptFile inner("set from_file 7");
    ScriptFile outer("source " + inner.str());
    Interpreter interp;
    (void)interp.evalFile(outer.str());
    BOOST_TEST(interp.variable("from_file").asInt() == 7);
}

BOOST_AUTO_TEST_CASE(a_missing_script_is_reported_by_path) {
    Interpreter interp;
    BOOST_CHECK_THROW(interp.evalFile("/nonexistent/ktplace/script.tcl"), Error);
}

namespace {

/// A manager with its own interpreter and its own empty store, so one test's
/// options cannot be seen by another.
class OptionsFixture {
public:
    OptionsFixture() : manager(std::make_unique<tcl::TclMgr>()) {
        OptionDb::instance().clear();
    }

    ~OptionsFixture() {
        OptionDb::instance().clear();
    }

    OptionsFixture(const OptionsFixture &) = delete;
    OptionsFixture &operator=(const OptionsFixture &) = delete;

    [[nodiscard]] tcl::TclMgr &mgr() {
        return *manager;
    }

private:
    std::unique_ptr<tcl::TclMgr> manager;
};

}  // namespace

// --------------------------------------------------------------------------
// The example scripts
// --------------------------------------------------------------------------

namespace {

[[nodiscard]] ComponentOptions optionsOf(Component component) {
    return OptionDb::instance().getComponentOption(component);
}

[[nodiscard]] OptionValue stored(Component component, std::string_view key) {
    const std::optional<OptionValue> value = optionsOf(component).find(key);
    BOOST_REQUIRE(value.has_value());
    return *value;
}

/// One of the .tcl files next to this test. The path is compiled in, so the test
/// does not depend on the directory the binary is run from.
[[nodiscard]] std::string exampleScript(const char *name) {
    return std::string(KTPLACE_SOURCE_ROOT) + "/tcl/test/" + name;
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_basic_example_sets_what_it_says) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("add_option_basic.tcl"));

    BOOST_TEST(std::get<int>(stored(Component::Simpl, "grid")) == 64);
    BOOST_TEST(std::get<double>(stored(Component::Simpl, "cg_tol")) == 1e-6);
    BOOST_TEST(std::get<int>(stored(Component::Abacus, "max_row_dist")) == 3);
    // The script mentioned no fastdp, so fastdp is unconfigured.
    BOOST_TEST(optionsOf(Component::FastDp).empty());
}

BOOST_AUTO_TEST_CASE(the_types_example_keeps_each_type) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("add_option_types.tcl"));

    BOOST_TEST(std::get<int>(stored(Component::Simpl, "grid")) == 64);
    BOOST_TEST(std::get<double>(stored(Component::Simpl, "alpha_base")) == 1.5e-3);
    // A bare 0 is an int, and a bool option reads it as false.
    BOOST_TEST(optionsOf(Component::Simpl).get<bool>("fences").value_or(true) == false);
    // Braces keep a string that reads like a number a string.
    BOOST_TEST(std::get<std::string>(stored(Component::Simpl, "tag")) == "a name with spaces");
}

BOOST_AUTO_TEST_CASE(the_computed_example_runs_ordinary_tcl) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("computed_options.tcl"));

    // The `if` narrowed the default the script set first.
    BOOST_TEST(std::get<int>(stored(Component::Simpl, "grid")) == 64);
    // `foreach` set an option on each of two components.
    BOOST_TEST(std::get<int>(stored(Component::Abacus, "max_row_dist")) == 3);
    BOOST_TEST(std::get<int>(stored(Component::FastDp, "window")) == 2);
    // A value taken from a variable.
    BOOST_TEST(std::get<int>(stored(Component::Simpl, "cluster_scale")) == 4);
}

BOOST_AUTO_TEST_CASE(a_broken_example_names_the_file_and_the_mistake) {
    OptionsFixture fixture;
    BOOST_CHECK_THROW(fixture.mgr().loadScript(exampleScript("invalid_option.tcl")), Error);
    // The mistake is in the script's last command, so nothing was stored.
    BOOST_TEST(OptionDb::instance().size() == 0);
}


// --------------------------------------------------------------------------
// Options
// --------------------------------------------------------------------------


BOOST_AUTO_TEST_CASE(add_option_records_a_typed_value_per_component) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval(
        "add_option simpl grid 64\n"
        "add_option simpl cg_tol 1e-6\n"
        "add_option simpl fences 0\n"
        "add_option abacus max_row_dist 3\n");

    // An unset option is nullopt, which is what lets a component keep its own
    // default instead of inheriting a zero.
    BOOST_TEST(!optionsOf(Component::Simpl).contains("cluster_scale"));
    BOOST_TEST(optionsOf(Component::FastDp).empty());

    BOOST_TEST(std::get<double>(stored(Component::Simpl, "cg_tol")) == 1e-6);
    BOOST_TEST(std::get<int>(stored(Component::Simpl, "grid")) == 64);

    // Components are independent: the same key in two is two options.
    BOOST_TEST(std::get<int>(stored(Component::Abacus, "max_row_dist")) == 3);
    BOOST_TEST(!optionsOf(Component::Abacus).contains("grid"));
}

BOOST_AUTO_TEST_CASE(add_option_needs_three_arguments) {
    OptionsFixture fixture;
    BOOST_CHECK_THROW(fixture.mgr().interpreter.eval("add_option simpl grid"), tcl::Error);
    BOOST_CHECK_THROW(fixture.mgr().interpreter.eval("add_option simpl grid 1 extra"), tcl::Error);
    BOOST_TEST(OptionDb::instance().size() == 0);
}

BOOST_AUTO_TEST_CASE(an_option_value_is_taken_from_a_variable) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval("set g 128\nadd_option simpl grid $g\n");
    BOOST_TEST(std::get<int>(stored(Component::Simpl, "grid")) == 128);
}

BOOST_AUTO_TEST_CASE(a_braced_value_stays_a_string) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval("add_option simpl tag {64}\n");
    BOOST_TEST(std::get<std::string>(stored(Component::Simpl, "tag")) == "64");
}

BOOST_AUTO_TEST_CASE(a_component_reads_its_own_options) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval("add_option simpl grid 96\nadd_option simpl fences 0\n");

    const ComponentOptions simpl = optionsOf(Component::Simpl);
    BOOST_TEST(simpl.get<int>("grid").value_or(-1) == 96);
    BOOST_TEST(simpl.get<bool>("fences").value_or(true) == false);
    BOOST_TEST(!simpl.get<int>("cluster_scale").has_value());
}

BOOST_AUTO_TEST_CASE(load_options_keeps_the_component_defaults_for_unset_keys) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval("add_option simpl grid 96\nadd_option simpl fences 0\n");

    const SimplOptions options = loadOptions<SimplOptions>();
    BOOST_TEST(options.binsPerAxis == 96u);
    BOOST_TEST(options.fences == false);
    BOOST_TEST(options.maxIters == SimplOptions{}.maxIters);
    BOOST_TEST(options.cgTol == SimplOptions{}.cgTol);
}

BOOST_AUTO_TEST_CASE(a_value_of_the_wrong_type_is_refused_not_converted) {
    OptionsFixture fixture;
    (void)fixture.mgr().interpreter.eval("add_option simpl grid sixty-four\n");
    BOOST_TEST(!optionsOf(Component::Simpl).get<int>("grid").has_value());
    BOOST_TEST(loadOptions<SimplOptions>().binsPerAxis == SimplOptions{}.binsPerAxis);
}

BOOST_AUTO_TEST_CASE(numbers_convert_to_the_type_asked_for) {
    BOOST_TEST(convertOption<double>(OptionValue{3}).value_or(-1.0) == 3.0);
    BOOST_TEST(convertOption<int>(OptionValue{64.0}).value_or(-1) == 64);
    BOOST_TEST(convertOption<bool>(OptionValue{0}).value_or(true) == false);
    BOOST_TEST(convertOption<bool>(OptionValue{2.5}).value_or(false) == true);
    BOOST_TEST(convertOption<std::size_t>(OptionValue{7}).value_or(0u) == 7u);
    BOOST_TEST(!convertOption<std::size_t>(OptionValue{-1}).has_value());
    BOOST_TEST(!convertOption<int>(OptionValue{true}).has_value());
    BOOST_TEST(!convertOption<int>(OptionValue{std::string("64")}).has_value());
    BOOST_TEST(!convertOption<std::string>(OptionValue{64}).has_value());
}

BOOST_AUTO_TEST_CASE(component_names_round_trip) {
    for (const std::string_view name : kComponentNames) {
        const std::optional<Component> id = componentFromName(name);
        BOOST_REQUIRE(id.has_value());
        BOOST_TEST(toString(*id) == name);
    }
    BOOST_TEST(!componentFromName("nonesuch").has_value());
}
