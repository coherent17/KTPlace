// SPDX-License-Identifier: BSD-3-Clause
// @file test_option.cc
// Unit tests for the command-line option parser.
//
// The parser has one job beyond reading flags: deciding where artifacts land.
// That resolution is the part worth pinning, because a relative path silently
// written to the wrong directory is the kind of bug that survives a test run and
// then loses a whole placement result.

#define BOOST_TEST_MODULE ktplace_option
#define BOOST_TEST_DYN_LINK

#include "kt_option.h"

#include "util/kt_log.h"

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace ktplace;

namespace {

/// Owns a throwaway directory and deletes it, so the tests never depend on the
/// build tree being writable or on leftovers from a previous run.
class ScratchDir {
public:
    explicit ScratchDir(const char *tag) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("ktplace_opt_" + std::string(tag) + "_" + std::to_string(++counter));
        std::filesystem::remove_all(path_);
    }

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::filesystem::path file(const std::string &name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

/// The design directory used throughout. It does not have to exist: these tests
/// exercise the parser, not the reader.
const char *kDesign = "./benchmark/ISPD_2005/adaptec1";

std::vector<char *> argvOf(std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    return argv;
}

kt_option parse(const std::vector<std::string> &args) {
    std::vector<std::string> copy = args;
    std::vector<char *> argv = argvOf(copy);
    kt_option opt;
    opt.parse(static_cast<int>(argv.size()), argv.data());
    return opt;
}

/// Redirects one standard stream into a string for the lifetime of the object.
class CaptureStream {
public:
    explicit CaptureStream(std::ostream &stream)
        : target(stream), original(stream.rdbuf(buffer.rdbuf())) {}
    ~CaptureStream() {
        target.rdbuf(original);
    }
    CaptureStream(const CaptureStream &) = delete;
    CaptureStream &operator=(const CaptureStream &) = delete;

    [[nodiscard]] std::string str() const {
        return buffer.str();
    }

private:
    std::ostream &target;
    std::ostringstream buffer;
    std::streambuf *original;
};

/// What one parse did: whether it said there is something to run, the fatal it
/// raised (if any), and what it wrote to each stream.
struct ParseOutcome {
    bool ready = false;
    bool fatal = false;
    std::string fatalMessage;
    std::string console;  // stderr, where every log record goes
    std::string stdoutText;
};

/// Parse a command line in-process, catching the FatalError a bad one raises.
ParseOutcome parseCapturing(const std::vector<std::string> &args) {
    std::vector<std::string> copy = args;
    std::vector<char *> argv = argvOf(copy);
    ParseOutcome out;
    CaptureStream err(std::cerr);
    CaptureStream sout(std::cout);
    try {
        kt_option opt;
        out.ready = opt.parse(static_cast<int>(argv.size()), argv.data());
    } catch (const FatalError &e) {
        out.fatal = true;
        out.fatalMessage = e.message();
    }
    out.console = err.str();
    out.stdoutText = sout.str();
    return out;
}

std::string cwd() {
    return std::filesystem::current_path().string();
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_input_directory_is_the_only_positional) {
    const kt_option opt = parse({"ktplace", kDesign, "-v"});
    BOOST_TEST(opt.inputPath == kDesign);
    BOOST_TEST(opt.verbose);
}

BOOST_AUTO_TEST_CASE(every_artifact_is_derived_from_the_work_dir) {
    const ScratchDir dir("derive");
    const std::string work = dir.file("run").string();
    const kt_option opt = parse({"ktplace", kDesign, "-w", work});

    BOOST_TEST(opt.workDir == work);
    // Fixed names: only the root moves with -w, so two designs can share one
    // directory without overwriting each other's frames.
    BOOST_TEST(opt.getOutputPath() == work + "/placed.pl");
    BOOST_TEST(opt.getPlotDir() == work + "/plots");
    BOOST_TEST(opt.getLogFile() == work + "/ktplace.log");
}

BOOST_AUTO_TEST_CASE(the_work_dir_defaults_to_the_current_directory) {
    const kt_option opt = parse({"ktplace", kDesign});
    BOOST_TEST(opt.workDir == cwd());
    BOOST_TEST(opt.getOutputPath() == cwd() + "/placed.pl");
    BOOST_TEST(opt.getLogFile() == cwd() + "/ktplace.log");
}

BOOST_AUTO_TEST_CASE(a_work_dir_is_created_when_asked_for) {
    const ScratchDir dir("create");
    const std::string work = (dir.file("a") / "b").string();
    BOOST_TEST(!std::filesystem::exists(work));
    parse({"ktplace", kDesign, "-w", work});
    BOOST_TEST(std::filesystem::is_directory(work));
}

BOOST_AUTO_TEST_CASE(an_uncreatable_work_dir_is_reported) {
    // A work dir under an existing *file* cannot be made. Reporting it beats
    // silently writing somewhere else.
    const ScratchDir dir("badwork");
    std::filesystem::create_directories(dir.file("blockerdir"));
    const std::string blocker = dir.file("blockerdir").string() + "/blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory\n";
    }
    BOOST_CHECK_THROW(parse({"ktplace", kDesign, "-w", blocker + "/under"}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(verbose_is_off_by_default) {
    BOOST_TEST(!parse({"ktplace", kDesign}).verbose);
}

BOOST_AUTO_TEST_CASE(the_placer_is_not_a_command_line_option) {
    // Choosing the placer belongs to the flow, which knows what placers exist, and
    // to the script. A flag here would put a component's vocabulary in the parser.
    const ParseOutcome run = parseCapturing({"ktplace", kDesign, "-a", "ntuplace1"});
    BOOST_TEST(run.fatal);
    BOOST_TEST(run.fatalMessage == "Unknown command-line option: -a");
}

BOOST_AUTO_TEST_CASE(every_flag_is_accepted_in_both_its_short_and_long_form) {
    // The work directory is created as it is parsed, so it goes in a scratch
    // directory: a bare relative name left an empty folder behind in whatever
    // directory the test ran from (src/ under `make test`, or the repository root).
    const ScratchDir dir("forms");
    const std::string work = dir.file("work").string();
    const struct {
        const char *shortForm;
        const char *longForm;
        const char *value;
    } forms[] = {{"-w", "--work-dir", work.c_str()}, {"-tcl", "--tcl", "run.tcl"}};

    for (const auto &f : forms) {
        BOOST_TEST_CONTEXT(f.shortForm) {
            BOOST_CHECK_NO_THROW(parse({"ktplace", kDesign, f.shortForm, f.value}));
            BOOST_CHECK_NO_THROW(parse({"ktplace", kDesign, f.longForm, f.value}));
        }
    }
}

BOOST_AUTO_TEST_CASE(flag_values_are_stored) {
    const kt_option opt = parse({"ktplace", kDesign, "-tcl", "run.tcl", "-v"});
    BOOST_TEST(opt.tclFile == "run.tcl");
    BOOST_TEST(opt.verbose);
}

BOOST_AUTO_TEST_CASE(verbose_has_a_long_form_too) {
    BOOST_TEST(parse({"ktplace", kDesign, "--verbose"}).verbose);
}

BOOST_AUTO_TEST_CASE(a_flag_at_the_end_without_a_value_is_rejected) {
    // Silently keeping the default would turn a typo into a run that writes the
    // wrong thing, so each of these has to throw.
    for (const char *flag : {"-w", "--work-dir", "-tcl", "--tcl"}) {
        BOOST_TEST_CONTEXT(flag) {
            BOOST_CHECK_THROW(parse({"ktplace", kDesign, flag}), std::runtime_error);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_tcl_script_is_kept_as_given) {
    // The script is loaded by the run, not by the parser, so parsing only has to
    // carry the path: resolving it here would create directories for a file that
    // may not exist.
    const kt_option opt = parse({"ktplace", kDesign, "-tcl", "cfg/run.tcl"});
    BOOST_TEST(opt.tclFile == "cfg/run.tcl");
    BOOST_TEST(parse({"ktplace", kDesign}).tclFile.empty());
}

BOOST_AUTO_TEST_CASE(an_unknown_flag_is_reported_and_stops_the_run) {
    const ParseOutcome run = parseCapturing({"ktplace", kDesign, "--nonesuch"});
    BOOST_TEST(run.fatal);
    BOOST_TEST(run.fatalMessage == "Unknown command-line option: --nonesuch");
    BOOST_TEST(run.console.find("Unknown command-line option") != std::string::npos);
    BOOST_TEST(run.console.find("--nonesuch") != std::string::npos);
    BOOST_TEST(run.stdoutText.empty());
}

BOOST_AUTO_TEST_CASE(no_input_directory_is_a_fatal_error) {
    const ParseOutcome run = parseCapturing({"ktplace"});
    BOOST_TEST(run.fatal);
    BOOST_TEST(run.fatalMessage == "Insufficient command-line arguments");
    BOOST_TEST(run.console.find("Insufficient command-line arguments") != std::string::npos);
    BOOST_TEST(run.stdoutText.empty());
}

BOOST_AUTO_TEST_CASE(help_and_version_stop_before_anything_else) {
    // Help has to win over a missing argument list, otherwise `ktplace --help`
    // would be an error instead of an answer.
    for (const char *flag : {"-h", "--help", "-V", "--version"}) {
        const ParseOutcome run = parseCapturing({"ktplace", flag});
        BOOST_TEST_CONTEXT(flag) {
            BOOST_TEST(!run.fatal);
            // A parser that reported Ready here would be claiming there is
            // something to run.
            BOOST_TEST(!run.ready);
            BOOST_TEST(!run.console.empty());
            BOOST_TEST(run.stdoutText.empty());
        }
    }
    BOOST_TEST(parseCapturing({"ktplace", "--help"}).console.find("Usage:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(the_parser_can_be_copied) {
    const kt_option src = parse({"ktplace", kDesign, "-v"});
    kt_option copy = src;
    BOOST_TEST(copy.inputPath == kDesign);
    BOOST_TEST(copy.verbose);

    copy.verbose = false;
    BOOST_TEST(src.verbose);
}

BOOST_AUTO_TEST_CASE(the_parser_can_be_moved) {
    kt_option src = parse({"ktplace", kDesign, "-v"});
    const kt_option dst = std::move(src);
    BOOST_TEST(dst.inputPath == kDesign);
    BOOST_TEST(dst.verbose);
}

BOOST_AUTO_TEST_CASE(parsing_twice_uses_the_second_command_line) {
    kt_option opt;
    std::vector<std::string> first{"ktplace", kDesign};
    std::vector<char *> fargv = argvOf(first);
    BOOST_TEST(opt.parse(static_cast<int>(fargv.size()), fargv.data()));

    std::vector<std::string> second{"ktplace", kDesign, "-v", "-tcl", "run.tcl"};
    std::vector<char *> sargv = argvOf(second);
    BOOST_TEST(opt.parse(static_cast<int>(sargv.size()), sargv.data()));
    BOOST_TEST(opt.verbose);
    BOOST_TEST(opt.tclFile == "run.tcl");
}

BOOST_AUTO_TEST_CASE(a_flow_script_makes_the_input_directory_optional) {
    for (const char *flag : {"-s", "--script"}) {
        BOOST_TEST_CONTEXT(flag) {
            const ParseOutcome run = parseCapturing({"ktplace", flag, "flow.tcl"});
            BOOST_TEST(!run.fatal);
            BOOST_TEST(run.ready);
        }
    }
    const kt_option opt = parse({"ktplace", "--script", "flow.tcl", kDesign, "-v"});
    BOOST_TEST(opt.script == "flow.tcl");
    BOOST_TEST(opt.inputPath == kDesign);
    BOOST_TEST(opt.verbose);
}

BOOST_AUTO_TEST_CASE(the_input_directory_may_follow_the_flags) {
    const kt_option opt = parse({"ktplace", "-tcl", "run.tcl", kDesign});
    BOOST_TEST(opt.inputPath == kDesign);
    BOOST_TEST(opt.tclFile == "run.tcl");
}

BOOST_AUTO_TEST_CASE(a_second_positional_is_rejected) {
    const ParseOutcome run = parseCapturing({"ktplace", kDesign, "extra"});
    BOOST_TEST(run.fatal);
    BOOST_TEST(run.fatalMessage == "Unknown command-line option: extra");
}

BOOST_AUTO_TEST_CASE(a_second_parse_forgets_the_first_script) {
    kt_option opt;
    std::vector<std::string> first{"ktplace", "--script", "flow.tcl", "-tcl", "run.tcl"};
    std::vector<char *> fargv = argvOf(first);
    BOOST_TEST(opt.parse(static_cast<int>(fargv.size()), fargv.data()));

    std::vector<std::string> second{"ktplace", kDesign};
    std::vector<char *> sargv = argvOf(second);
    BOOST_TEST(opt.parse(static_cast<int>(sargv.size()), sargv.data()));
    BOOST_TEST(opt.script.empty());
    BOOST_TEST(opt.tclFile.empty());
    BOOST_TEST(opt.inputPath == kDesign);
}

BOOST_AUTO_TEST_CASE(gui_hands_every_other_argument_to_the_window) {
    std::vector<std::string> args{"ktplace", kDesign, "--gui", "-w", "out", "--script", "flow.tcl"};
    std::vector<char *> argv = argvOf(args);
    const auto gui = kt_option::guiArguments(static_cast<int>(argv.size()), argv.data());
    BOOST_REQUIRE(gui.has_value());
    const std::vector<std::string> expected{kDesign, "-w", "out", "--script", "flow.tcl"};
    BOOST_TEST(*gui == expected, boost::test_tools::per_element());

    // Alone, it opens an empty window.
    std::vector<std::string> bare{"ktplace", "--gui"};
    std::vector<char *> bargv = argvOf(bare);
    const auto empty = kt_option::guiArguments(static_cast<int>(bargv.size()), bargv.data());
    BOOST_REQUIRE(empty.has_value());
    BOOST_TEST(empty->empty());
}

BOOST_AUTO_TEST_CASE(without_gui_or_with_help_the_cli_answers_itself) {
    for (const std::vector<std::string> &line :
         {std::vector<std::string>{"ktplace", kDesign, "-v"},
          std::vector<std::string>{"ktplace", "--gui", "--help"},
          std::vector<std::string>{"ktplace", "-V", "--gui"}}) {
        std::vector<std::string> copy = line;
        std::vector<char *> argv = argvOf(copy);
        BOOST_TEST(!kt_option::guiArguments(static_cast<int>(argv.size()), argv.data()));
    }
}

BOOST_AUTO_TEST_CASE(the_gui_binary_is_looked_for_beside_the_running_one) {
    const std::filesystem::path gui = kt_option::guiExecutable("ignored-on-linux");
    BOOST_TEST(gui.filename() == "ktplace-gui");
    // On Linux the directory comes from /proc/self/exe: this test binary's own.
    std::error_code ec;
    const std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        BOOST_TEST(gui.parent_path() == self.parent_path());
    }
}
