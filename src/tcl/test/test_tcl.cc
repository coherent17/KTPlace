// SPDX-License-Identifier: BSD-3-Clause
// @file test_tcl.cc
// Unit tests for the Tcl interpreter and the option registry.

#define BOOST_TEST_MODULE ktplace_tcl

#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>
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

namespace ktplace {
std::ostream &operator<<(std::ostream &out, OptionOrigin origin) {
    return out << toString(origin);
}
}  // namespace ktplace

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
    interp.eval("set name simpl");
    BOOST_TEST(interp.eval("set a $name").asString() == "simpl");
    BOOST_TEST(interp.eval("set b {$name}").asString() == "$name");
    BOOST_TEST(interp.eval("set c \"$name\"").asString() == "simpl");
}

BOOST_AUTO_TEST_CASE(command_substitution_composes) {
    Interpreter interp;
    const Value out = interp.eval("set total [expr {2 + 3 * 4}]");
    BOOST_TEST(out.asInt() == 14);
    interp.eval("set x [list a b]");
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
    interp.eval("set n 0\nif {$n == 0} { set n 5 } else { set n 9 }");
    BOOST_TEST(interp.variable("n").asInt() == 5);
    interp.eval("set i 0\nwhile {$i < 4} { incr i 2 }");
    BOOST_TEST(interp.variable("i").asInt() == 4);
}

BOOST_AUTO_TEST_CASE(foreach_walks_a_list) {
    Interpreter interp;
    interp.eval("set total 0\nforeach x {1 2 3 4} { incr total $x }");
    BOOST_TEST(interp.variable("total").asInt() == 10);
    interp.eval("set joined {}\nforeach {k v} {a 1 b 2} { lappend joined $k$v }");
    BOOST_TEST(interp.variable("joined").asString() == "{a1 b2}");
}

BOOST_AUTO_TEST_CASE(a_multiline_script_is_separated_by_newlines) {
    Interpreter interp;
    interp.eval("set a 1\nset b 2 ; set c 3\n");
    BOOST_TEST(interp.variable("a").asInt() == 1);
    BOOST_TEST(interp.variable("c").asInt() == 3);
}

BOOST_AUTO_TEST_CASE(comments_and_continuations_do_not_become_commands) {
    Interpreter interp;
    interp.eval("# leading comment\nset a 1\n# final");
    BOOST_TEST(interp.variable("a").asInt() == 1);
    BOOST_CHECK_THROW(interp.eval("set a 1 # trailing"), Error);
    // A bracketed body may run over lines without becoming two commands.
    interp.eval("set list {}\nlappend list \\\n    2\n");
    BOOST_TEST(interp.variable("list").asList().size() == 1);
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
    interp.evalFile(outer.str());
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

/// One of the .tcl files next to this test. The path is compiled in, so the test
/// does not depend on the directory the binary is run from.
[[nodiscard]] std::string exampleScript(const char *name) {
    return std::string(KTPLACE_SOURCE_ROOT) + "/tcl/test/" + name;
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_basic_example_sets_what_it_says) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("add_option_basic.tcl"));

    BOOST_TEST(std::get<int>(*OptionDb::instance().find("simpl", "grid")) == 64);
    BOOST_TEST(std::get<double>(*OptionDb::instance().find("simpl", "cg_tol")) == 1e-6);
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("abacus", "max_row_dist")) == 3);
    // The script mentioned no fastdp, so fastdp is unconfigured.
    BOOST_TEST(!OptionDb::instance().optionsFor("fastdp").has_value());
}

BOOST_AUTO_TEST_CASE(the_types_example_keeps_each_type) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("add_option_types.tcl"));

    BOOST_TEST(std::get<int>(*OptionDb::instance().find("simpl", "grid")) == 64);
    BOOST_TEST(std::get<double>(*OptionDb::instance().find("simpl", "alpha_base")) == 1.5e-3);
    // A bare 0 is an int, and a bool option reads it as false.
    const Option<bool> fences("simpl", "fences", true);
    BOOST_TEST(fences.value() == false);
    // Braces keep a string that reads like a number a string.
    BOOST_TEST(std::get<std::string>(*OptionDb::instance().find("simpl", "tag")) ==
               "a name with spaces");
}

BOOST_AUTO_TEST_CASE(the_computed_example_runs_ordinary_tcl) {
    OptionsFixture fixture;
    fixture.mgr().loadScript(exampleScript("computed_options.tcl"));

    // The `if` narrowed the default the script set first.
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("simpl", "grid")) == 64);
    // `foreach` set an option on each of two components.
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("abacus", "max_row_dist")) == 3);
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("fastdp", "window")) == 2);
    // A value taken from a variable.
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("simpl", "cluster_scale")) == 4);
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
    fixture.mgr().interpreter.eval(
        "add_option simpl grid 64\n"
        "add_option simpl cg_tol 1e-6\n"
        "add_option simpl fences 0\n"
        "add_option abacus max_row_dist 3\n");

    // An unset option is nullopt, which is what lets a component keep its own
    // default instead of inheriting a zero.
    BOOST_TEST(!OptionDb::instance().find("simpl", "cluster_scale").has_value());
    BOOST_TEST(!OptionDb::instance().optionsFor("fastdp").has_value());

    const auto tol = OptionDb::instance().find("simpl", "cg_tol");
    BOOST_REQUIRE(tol.has_value());
    BOOST_TEST(std::get<double>(*tol) == 1e-6);

    const auto grid = OptionDb::instance().find("simpl", "grid");
    BOOST_REQUIRE(grid.has_value());
    BOOST_TEST(std::get<int>(*grid) == 64);

    // Components are independent: the same key in two is two options.
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("abacus", "max_row_dist")) == 3);
    BOOST_TEST(!OptionDb::instance().find("abacus", "grid").has_value());
}

BOOST_AUTO_TEST_CASE(add_option_needs_three_arguments) {
    OptionsFixture fixture;
    BOOST_CHECK_THROW(fixture.mgr().interpreter.eval("add_option simpl grid"), tcl::Error);
    BOOST_CHECK_THROW(fixture.mgr().interpreter.eval("add_option simpl grid 1 extra"), tcl::Error);
    BOOST_TEST(OptionDb::instance().size() == 0);
}

BOOST_AUTO_TEST_CASE(an_option_value_is_taken_from_a_variable) {
    OptionsFixture fixture;
    fixture.mgr().interpreter.eval("set g 128\nadd_option simpl grid $g\n");
    BOOST_TEST(std::get<int>(*OptionDb::instance().find("simpl", "grid")) == 128);
}

BOOST_AUTO_TEST_CASE(a_braced_value_stays_a_string) {
    OptionsFixture fixture;
    fixture.mgr().interpreter.eval("add_option simpl tag {64}\n");
    const auto tag = OptionDb::instance().find("simpl", "tag");
    BOOST_REQUIRE(tag.has_value());
    BOOST_TEST(std::get<std::string>(*tag) == "64");
}

BOOST_AUTO_TEST_CASE(a_component_reads_its_own_options) {
    OptionsFixture fixture;
    fixture.mgr().interpreter.eval("add_option simpl grid 96\nadd_option simpl fences 0\n");

    // The typed view is what a component declares: key, type and default together.
    const Option<int> grid("simpl", "grid", 64);
    const Option<bool> fences("simpl", "fences", true);
    const Option<int> unconfigured("simpl", "cluster_scale", 2);

    BOOST_TEST(grid.value() == 96);
    BOOST_TEST(grid.isSet());
    BOOST_TEST(fences.value() == false);
    // Unset falls back to the component's own default, and says so.
    BOOST_TEST(unconfigured.value() == 2);
    BOOST_TEST(unconfigured.isSet() == false);
    BOOST_TEST(unconfigured.origin() == OptionOrigin::Default);
    BOOST_TEST(grid.origin() == OptionOrigin::Script);
}

BOOST_AUTO_TEST_CASE(a_value_of_the_wrong_type_is_refused_not_converted) {
    OptionsFixture fixture;
    fixture.mgr().interpreter.eval("add_option simpl grid sixty-four\n");
    // A string where an int was expected is not silently read as 0.
    const Option<int> grid("simpl", "grid", 64);
    BOOST_TEST(grid.isSet() == false);
    BOOST_TEST(grid.value() == 64);
}
