// @file test_tclShell.cc
// Tests for TclShell: the commands driving a ktSession on a tiny synthetic
// design, errors coming back as Tcl errors rather than ending the process, and
// the logging rules (command echo, `puts` kept off stdout).

#define BOOST_TEST_MODULE ktplace_tcl_shell
#define BOOST_TEST_DYN_LINK
#include "datamodel/kt_optionDb.h"
#include "session/kt_session.h"
#include "tcl/kt_tclShell.h"
#include "test/kt_testDesign.h"
#include "util/kt_progress.h"

#include <algorithm>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace ktplace;
using ktplace::testing::countNonEmptyLines;
using ktplace::testing::designDir;
using ktplace::testing::ScratchDir;
using ktplace::testing::writeBookshelfDesign;
namespace fs = std::filesystem;

namespace {

/// Redirects one standard stream into a string for the lifetime of the object.
class Capture {
public:
    explicit Capture(std::ostream &s) : stream(s), original(s.rdbuf(buffer.rdbuf())) {}
    ~Capture() {
        stream.rdbuf(original);
    }
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    [[nodiscard]] std::string str() const {
        return buffer.str();
    }

private:
    std::ostream &stream;
    std::ostringstream buffer;
    std::streambuf *original;
};

/// A shell over a fresh session, quiet on stderr unless a test captures it.
struct Fixture {
    ktSession session;
    TclShell shell{session};
};

/// Evaluate and require success, reporting Tcl's trace if it failed.
std::string ok(TclShell &shell, const std::string &script) {
    const TclShell::Result r = shell.eval(script);
    BOOST_REQUIRE_MESSAGE(r.ok, "`" << script << "` failed: " << r.trace);
    return r.text;
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_shell_registers_the_session_commands) {
    Fixture f;
    const std::vector<std::string> names = f.shell.commandNames();
    for (const char *expected :
         {"read_design", "report_design", "write_design", "global_place", "legalize",
          "detail_place", "report_hpwl", "check_placement", "report_timing", "set_plot_dir",
          "write_final_image", "write_animation", "add_option", "get_option", "report_options",
          "help"}) {
        BOOST_TEST_CONTEXT(expected) {
            BOOST_TEST((std::ranges::find(names, expected) != names.end()));
        }
    }
    BOOST_TEST(std::ranges::is_sorted(names));
    // Tcl's own commands are still there.
    BOOST_TEST(ok(f.shell, "expr {6 * 7}") == "42");
}

BOOST_AUTO_TEST_CASE(a_script_runs_the_whole_flow) {
    const ScratchDir dir("tcl_flow");
    writeBookshelfDesign(dir);
    Fixture f;
    f.shell.setGlobal("input_dir", designDir(dir).string());
    f.shell.setGlobal("output", dir.file("placed.pl").string());

    Capture err(std::cerr);
    ok(f.shell, "read_design $ktplace(input_dir)");
    BOOST_TEST(f.session.hasDesign());
    ok(f.shell, "global_place -algo simpl");
    ok(f.shell, "legalize");
    BOOST_TEST(ok(f.shell, "check_placement").empty());  // legal: no defects
    ok(f.shell, "detail_place");
    BOOST_TEST(ok(f.shell, "llength [check_placement {after detail}]") == "0");

    const double hpwl = std::stod(ok(f.shell, "report_hpwl"));
    BOOST_TEST(hpwl == f.session.hpwl());
    BOOST_TEST(hpwl > 0.0);

    ok(f.shell, "write_design $ktplace(output)");
    BOOST_TEST(countNonEmptyLines(dir.file("placed.pl")) >= f.session.cells().size());
}

BOOST_AUTO_TEST_CASE(a_failing_stage_is_a_tcl_error_and_the_shell_carries_on) {
    Fixture f;
    Capture err(std::cerr);
    const TclShell::Result r = f.shell.eval("legalize");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.text.find("legalize: no design loaded") != std::string::npos);
    BOOST_TEST(r.trace.find("legalize") != std::string::npos);
    // The fatal record was logged where it was raised.
    BOOST_TEST(err.str().find("[fatal]") != std::string::npos);

    // The same interpreter keeps working, and a script can catch the error.
    BOOST_TEST(ok(f.shell, "expr {1 + 1}") == "2");
    BOOST_TEST(ok(f.shell, "catch {detail_place} msg") == "1");
    BOOST_TEST(ok(f.shell, "set msg").find("detail_place: no design loaded") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_missing_input_directory_is_a_tcl_error) {
    const ScratchDir dir("tcl_badinput");
    Fixture f;
    Capture err(std::cerr);
    const TclShell::Result r = f.shell.eval("read_design " + dir.str() + "/no_such_dir");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.text.find("cannot read input directory") != std::string::npos);
    BOOST_TEST(!f.session.hasDesign());
}

BOOST_AUTO_TEST_CASE(wrong_arguments_name_the_expected_form) {
    Fixture f;
    const TclShell::Result missing = f.shell.eval("read_design");
    BOOST_TEST(!missing.ok);
    BOOST_TEST(missing.text == "wrong # args: should be \"read_design <dir>\"");

    const TclShell::Result extra = f.shell.eval("legalize now");
    BOOST_TEST(!extra.ok);
    BOOST_TEST(extra.text == "wrong # args: should be \"legalize\"");

    const TclShell::Result option = f.shell.eval("global_place -algorithm simpl");
    BOOST_TEST(!option.ok);
    BOOST_TEST(option.text.find("unknown option \"-algorithm\"") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(puts_goes_to_the_log_never_to_stdout) {
    const ScratchDir dir("tcl_puts");
    Fixture f;
    Capture out(std::cout);
    Capture err(std::cerr);
    ok(f.shell, "puts hello-from-tcl");
    ok(f.shell, "puts stderr warned-from-tcl");
    ok(f.shell, "puts -nonewline stdout partial-from-tcl");
    BOOST_TEST(out.str().empty());
    BOOST_TEST(err.str().find("hello-from-tcl") != std::string::npos);
    BOOST_TEST(err.str().find("warned-from-tcl") != std::string::npos);
    BOOST_TEST(err.str().find("partial-from-tcl") != std::string::npos);

    // A file channel is still Tcl's.
    const std::string file = dir.file("out.txt").string();
    ok(f.shell, "set fh [open {" + file + "} w]; puts $fh line-one; close $fh");
    std::ifstream in(file);
    std::string line;
    std::getline(in, line);
    BOOST_TEST(line == "line-one");

    BOOST_TEST(!f.shell.eval("puts").ok);
}

BOOST_AUTO_TEST_CASE(each_top_level_command_is_echoed_to_the_log) {
    Fixture f;
    Capture err(std::cerr);
    ok(f.shell, "set answer 42\nproc twice {x} { expr {$x * 2} }\ntwice $answer");
    const std::string log = err.str();
    BOOST_TEST(log.find("tcl> set answer 42") != std::string::npos);
    BOOST_TEST(log.find("tcl> twice $answer") != std::string::npos);
    // The procedure's body runs a level down and is not echoed.
    BOOST_TEST(log.find("tcl> expr") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_script_file_error_names_its_line) {
    const ScratchDir dir("tcl_file");
    const std::string script = dir.file("broken.tcl").string();
    {
        std::ofstream out(script);
        out << "set x 1\nlegalize\nset y 2\n";
    }
    Fixture f;
    Capture err(std::cerr);
    const TclShell::Result r = f.shell.evalFile(script);
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.text.find("no design loaded") != std::string::npos);
    BOOST_TEST(r.trace.find("line 2") != std::string::npos);
    // The line after the failure did not run.
    BOOST_TEST(!f.shell.eval("set y").ok);

    const TclShell::Result missing = f.shell.evalFile(dir.file("absent.tcl").string());
    BOOST_TEST(!missing.ok);
    BOOST_TEST(missing.text.find("couldn't read file") != std::string::npos);

    // `return` at the top of a script ends it normally.
    {
        std::ofstream out(dir.file("early.tcl"));
        out << "set z 1\nreturn\nlegalize\n";
    }
    BOOST_TEST(f.shell.evalFile(dir.file("early.tcl").string()).ok);
}

BOOST_AUTO_TEST_CASE(globals_reach_the_ktplace_array) {
    Fixture f;
    f.shell.setGlobal("work_dir", "/some/where");
    BOOST_TEST(ok(f.shell, "set ktplace(work_dir)") == "/some/where");
    BOOST_TEST(ok(f.shell, "proc wd {} { global ktplace; return $ktplace(work_dir) }; wd") ==
               "/some/where");
}

BOOST_AUTO_TEST_CASE(incomplete_commands_are_recognised) {
    BOOST_TEST(TclShell::isComplete("set x 1"));
    BOOST_TEST(TclShell::isComplete("proc a {} {\n  return 1\n}"));
    BOOST_TEST(!TclShell::isComplete("proc a {} {"));
    BOOST_TEST(!TclShell::isComplete("puts \"unterminated"));
}

BOOST_AUTO_TEST_CASE(help_describes_the_commands) {
    Fixture f;
    Capture err(std::cerr);
    ok(f.shell, "help");
    BOOST_TEST(err.str().find("read_design <dir>") != std::string::npos);
    ok(f.shell, "help legalize");
    BOOST_TEST(!f.shell.eval("help nonesuch").ok);
}

BOOST_AUTO_TEST_CASE(the_plot_directory_can_be_turned_off) {
    const ScratchDir dir("tcl_plots");
    writeBookshelfDesign(dir);
    Fixture f;
    Capture err(std::cerr);
    ok(f.shell, "set_plot_dir \"\"");
    BOOST_TEST(!f.session.plotOptions().enabled());
    ok(f.shell, "read_design {" + designDir(dir).string() + "}");
    ok(f.shell, "legalize");
    ok(f.shell, "write_final_image");
    BOOST_TEST(ok(f.shell, "write_animation") == "0");

    const std::string plots = dir.file("plots").string();
    ok(f.shell, "set_plot_dir {" + plots + "}");
    BOOST_TEST(f.session.plotOptions().dir == plots);
}

BOOST_AUTO_TEST_CASE(a_sourced_script_is_echoed_but_procedure_bodies_are_not) {
    const ScratchDir dir("tcl_source");
    const std::string inner = dir.file("inner.tcl").string();
    const std::string outer = dir.file("outer.tcl").string();
    {
        std::ofstream out(inner);
        out << "set from_inner 1\nproc triple {x} { expr {$x * 3} }\ntriple 4\n";
    }
    {
        std::ofstream out(outer);
        out << "set from_outer 1\nsource {" << inner << "}\nset after_inner 1\n";
    }
    Fixture f;
    Capture err(std::cerr);
    ok(f.shell, "source {" + outer + "}");
    const std::string log = err.str();
    for (const char *line : {"tcl> set from_outer 1", "tcl> set from_inner 1", "tcl> triple 4",
                             "tcl> set after_inner 1"}) {
        BOOST_TEST_CONTEXT(line) {
            BOOST_TEST(log.find(line) != std::string::npos);
        }
    }
    BOOST_TEST(log.find("tcl> expr") == std::string::npos);
    BOOST_TEST(log.find("::tcl::kt_") == std::string::npos);
    BOOST_TEST(ok(f.shell, "triple 5") == "15");

    // A procedure that sources a file: its body is not echoed, nor is the file.
    Capture again(std::cerr);
    ok(f.shell, "proc load_it {} { source {" + inner + "} }; load_it");
    BOOST_TEST(again.str().find("tcl> set from_inner") == std::string::npos);

    // The other forms still reach Tcl's own source, and errors keep their line.
    ok(f.shell, "source -encoding utf-8 {" + inner + "}");
    {
        std::ofstream out(dir.file("bad.tcl"));
        out << "set ok 1\nlegalize\n";
    }
    const TclShell::Result r = f.shell.eval("source {" + dir.file("bad.tcl").string() + "}");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.trace.find("line 2") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_stop_request_fails_the_commands_after_it) {
    const ScratchDir dir("tcl_stop");
    writeBookshelfDesign(dir);
    Fixture f;
    f.shell.setGlobal("input_dir", designDir(dir).string());
    Capture err(std::cerr);
    ok(f.shell, "read_design $ktplace(input_dir)");

    Progress::instance().requestStop();
    const TclShell::Result r = f.shell.eval("legalize; detail_place");
    BOOST_TEST(!r.ok);
    BOOST_TEST(r.text == "legalize: stopped on request");
    // Tcl itself is not stopped, only the KTPlace commands.
    BOOST_TEST(ok(f.shell, "expr {2 + 2}") == "4");

    Progress::instance().clearStop();
    ok(f.shell, "legalize");
}

BOOST_AUTO_TEST_CASE(options_come_from_scripts_and_never_from_the_environment) {
    Fixture f;
    OptionDb::instance().clear();

    // Nothing sets it: the stage's own default applies.
    BOOST_TEST(ok(f.shell, "get_option simpl grid").empty());
    BOOST_TEST(!OptionDb::instance().optionsFor("simpl").has_value());

    // The environment is not a source of options, under any spelling.
    ::setenv("KTPLACE_SIMPL_GRID", "48", 1);
    ::setenv("SIMPL_GRID", "48", 1);
    BOOST_TEST(ok(f.shell, "get_option simpl grid").empty());
    BOOST_TEST(!OptionDb::instance().originOf("simpl", "grid").has_value());
    ::unsetenv("KTPLACE_SIMPL_GRID");
    ::unsetenv("SIMPL_GRID");

    ok(f.shell, "add_option simpl grid 128");
    BOOST_TEST(ok(f.shell, "get_option simpl grid") == "128");
    BOOST_TEST((OptionDb::instance().originOf("simpl", "grid") == OptionOrigin::Script));
    // A later add_option replaces the value.
    ok(f.shell, "add_option simpl grid 96");
    BOOST_TEST(ok(f.shell, "get_option simpl grid") == "96");

    // Typed by spelling, as the configuration interpreter types them.
    ok(f.shell,
       "add_option simpl cg_tol 1e-6; add_option simpl fences false; "
       "add_option simpl start input");
    BOOST_TEST((std::holds_alternative<double>(*OptionDb::instance().find("simpl", "cg_tol"))));
    BOOST_TEST((std::holds_alternative<bool>(*OptionDb::instance().find("simpl", "fences"))));
    BOOST_TEST(ok(f.shell, "get_option simpl start") == "input");
    BOOST_TEST(!f.shell.eval("add_option simpl grid").ok);
    BOOST_TEST(!f.shell.eval("add_option {} grid 1").ok);
    {
        Capture err(std::cerr);
        ok(f.shell, "report_options");
        BOOST_TEST(err.str().find("Configured options") != std::string::npos);
    }
    OptionDb::instance().clear();
}
