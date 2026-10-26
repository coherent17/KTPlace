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

#include <boost/test/unit_test.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
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
        tempDir = std::filesystem::temp_directory_path() /
                  ("ktplace_opt_" + std::string(tag) + "_" + std::to_string(++counter));
        std::filesystem::remove_all(tempDir);
    }

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(tempDir, ec);
    }

    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::filesystem::path getFile(const std::string &name) const {
        return tempDir / name;
    }

private:
    std::filesystem::path tempDir;
};

/// The design directory used throughout. It does not have to exist: these tests
/// exercise the parser, not the reader.
const char *design = "./benchmark/bookshelf/ISPD_2005/adaptec1";

std::vector<char *> getArgvOf(std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    return argv;
}

kt_option parse(const std::vector<std::string> &args) {
    std::vector<std::string> copy = args;
    std::vector<char *> argv = getArgvOf(copy);
    kt_option opt;
    opt.parse(static_cast<int>(argv.size()), argv.data());
    return opt;
}

/// What a forked child did: its exit status, and everything it wrote to stdout and
/// stderr together.
struct ChildRun {
    int status = 0;
    std::string output;
};

/// Runs `body` in a child process and collects its output. Needed for the paths
/// that go through ktlog.fatal, which is [[noreturn]] and exits, so the parent
/// could not observe them any other way.
template <typename Fn>
ChildRun runInChild(Fn body) {
    int channel[2];
    BOOST_REQUIRE_EQUAL(::pipe(channel), 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(channel[0]);
        ::dup2(channel[1], STDOUT_FILENO);
        ::dup2(channel[1], STDERR_FILENO);
        ::close(channel[1]);
        body();
        std::fflush(nullptr);
        ::_exit(EXIT_SUCCESS);  // only reached if fatal() did not terminate
    }
    ::close(channel[1]);
    ChildRun run;
    char buffer[1024];
    ssize_t got = 0;
    while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        run.output.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(channel[0]);
    ::waitpid(pid, &run.status, 0);
    return run;
}

/// Parse a command line in a child, since a bad one may exit rather than throw.
ChildRun parseInChild(const std::vector<std::string> &args) {
    std::vector<std::string> copy = args;
    std::vector<char *> argv = getArgvOf(copy);
    return runInChild([&argv] {
        kt_option opt;
        opt.parse(static_cast<int>(argv.size()), argv.data());
    });
}

std::string getCwd() {
    return std::filesystem::current_path().string();
}

}  // namespace

BOOST_AUTO_TEST_CASE(the_input_directory_is_the_only_positional) {
    const kt_option opt = parse({"ktplace", design, "-v"});
    BOOST_TEST(opt.inputPath == design);
    BOOST_TEST(opt.verbose);
}

BOOST_AUTO_TEST_CASE(the_cli_is_not_the_gui_unless_asked) {
    BOOST_TEST((parse({"ktplace", design}).mode == Mode::Batch));
}

BOOST_AUTO_TEST_CASE(gui_takes_no_input_directory_or_one_anywhere_in_the_line) {
    const kt_option bare = parse({"ktplace", "--gui"});
    BOOST_TEST((bare.mode == Mode::Gui));
    BOOST_TEST(bare.inputPath.empty());

    const ScratchDir dir("gui");
    const std::string work = dir.getFile("run").string();
    const kt_option flagsOnly = parse({"ktplace", "--gui", "-w", work, "-tcl", "run.tcl"});
    BOOST_TEST(flagsOnly.inputPath.empty());
    BOOST_TEST(flagsOnly.workDir == work);
    BOOST_TEST(flagsOnly.tclFile == "run.tcl");

    const kt_option trailing = parse({"ktplace", design, "-w", work, "--gui"});
    BOOST_TEST((trailing.mode == Mode::Gui));
    BOOST_TEST(trailing.inputPath == design);
    BOOST_TEST(trailing.workDir == work);
}

BOOST_AUTO_TEST_CASE(the_shell_is_its_own_mode) {
    BOOST_TEST((parse({"ktplace", "--shell"}).mode == Mode::Shell));
    BOOST_TEST((parse({"ktplace", "--shell", "--shell"}).mode == Mode::Shell));
}

BOOST_AUTO_TEST_CASE(gui_and_shell_cannot_be_combined) {
    const ChildRun run = parseInChild({"ktplace", "--gui", "--shell"});
    BOOST_TEST(WIFEXITED(run.status));
    BOOST_TEST(WEXITSTATUS(run.status) == EXIT_FAILURE);
    BOOST_TEST(run.output.find("--gui and --shell cannot be combined") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(every_artifact_is_derived_from_the_work_dir) {
    const ScratchDir dir("derive");
    const std::string work = dir.getFile("run").string();
    const kt_option opt = parse({"ktplace", design, "-w", work});

    BOOST_TEST(opt.workDir == work);
    // Fixed names: only the root moves with -w, so two designs can share one
    // directory without overwriting each other's frames.
    BOOST_TEST(opt.getOutputPath() == work + "/placed.pl");
    BOOST_TEST(opt.getPlotDir() == work + "/plots");
    BOOST_TEST(opt.getLogFile() == work + "/ktplace.log");
}

BOOST_AUTO_TEST_CASE(the_work_dir_defaults_to_the_current_directory) {
    const kt_option opt = parse({"ktplace", design});
    BOOST_TEST(opt.workDir == getCwd());
    BOOST_TEST(opt.getOutputPath() == getCwd() + "/placed.pl");
    BOOST_TEST(opt.getLogFile() == getCwd() + "/ktplace.log");
}

BOOST_AUTO_TEST_CASE(a_work_dir_is_created_when_asked_for) {
    const ScratchDir dir("create");
    const std::string work = (dir.getFile("a") / "b").string();
    BOOST_TEST(!std::filesystem::exists(work));
    parse({"ktplace", design, "-w", work});
    BOOST_TEST(std::filesystem::is_directory(work));
}

BOOST_AUTO_TEST_CASE(an_uncreatable_work_dir_is_reported) {
    // A work dir under an existing *file* cannot be made. Reporting it beats
    // silently writing somewhere else.
    const ScratchDir dir("badwork");
    std::filesystem::create_directories(dir.getFile("blockerdir"));
    const std::string blocker = dir.getFile("blockerdir").string() + "/blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory\n";
    }
    BOOST_CHECK_THROW(parse({"ktplace", design, "-w", blocker + "/under"}), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(verbose_is_off_by_default) {
    BOOST_TEST(!parse({"ktplace", design}).verbose);
}

BOOST_AUTO_TEST_CASE(the_placer_is_not_a_command_line_option) {
    // Choosing the placer belongs to the flow, which knows what placers exist, and
    // to the script. A flag here would put a component's vocabulary in the parser.
    const ChildRun run = parseInChild({"ktplace", design, "-a", "ntuplace1"});
    BOOST_TEST(WIFEXITED(run.status));
    BOOST_TEST(WEXITSTATUS(run.status) == EXIT_FAILURE);
    BOOST_TEST(run.output.find("Unknown command-line option") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(every_flag_is_accepted_in_both_its_short_and_long_form) {
    // The work directory is created as it is parsed, so it goes in a scratch
    // directory: a bare relative name left an empty folder behind in whatever
    // directory the test ran from (src/ under `make test`, or the repository root).
    const ScratchDir dir("forms");
    const std::string work = dir.getFile("work").string();
    const struct {
        const char *shortForm;
        const char *longForm;
        const char *value;
    } forms[] = {{.shortForm = "-w", .longForm = "--work-dir", .value = work.c_str()},
                 {.shortForm = "-tcl", .longForm = "--tcl", .value = "run.tcl"}};

    for (const auto &f : forms) {
        BOOST_TEST_CONTEXT(f.shortForm) {
            BOOST_CHECK_NO_THROW(parse({"ktplace", design, f.shortForm, f.value}));
            BOOST_CHECK_NO_THROW(parse({"ktplace", design, f.longForm, f.value}));
        }
    }
}

BOOST_AUTO_TEST_CASE(flag_values_are_stored) {
    const kt_option opt = parse({"ktplace", design, "-tcl", "run.tcl", "-v"});
    BOOST_TEST(opt.tclFile == "run.tcl");
    BOOST_TEST(opt.verbose);
}

BOOST_AUTO_TEST_CASE(verbose_has_a_long_form_too) {
    BOOST_TEST(parse({"ktplace", design, "--verbose"}).verbose);
}

BOOST_AUTO_TEST_CASE(a_flag_at_the_end_without_a_value_is_rejected) {
    // Silently keeping the default would turn a typo into a run that writes the
    // wrong thing, so each of these has to throw.
    for (const char *flag : {"-w", "--work-dir", "-tcl", "--tcl"}) {
        BOOST_TEST_CONTEXT(flag) {
            BOOST_CHECK_THROW(parse({"ktplace", design, flag}), std::runtime_error);
        }
    }
}

BOOST_AUTO_TEST_CASE(a_tcl_script_is_kept_as_given) {
    // The script is loaded by the run, not by the parser, so parsing only has to
    // carry the path: resolving it here would create directories for a file that
    // may not exist.
    const kt_option opt = parse({"ktplace", design, "-tcl", "cfg/run.tcl"});
    BOOST_TEST(opt.tclFile == "cfg/run.tcl");
    BOOST_TEST(parse({"ktplace", design}).tclFile.empty());
}

BOOST_AUTO_TEST_CASE(an_unknown_flag_is_reported_and_stops_the_run) {
    const ChildRun run = parseInChild({"ktplace", design, "--nonesuch"});
    BOOST_TEST(WIFEXITED(run.status));
    BOOST_TEST(WEXITSTATUS(run.status) == EXIT_FAILURE);
    BOOST_TEST(run.output.find("Unknown command-line option") != std::string::npos);
    BOOST_TEST(run.output.find("--nonesuch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(no_input_directory_is_a_fatal_error) {
    // parse reports through ktlog.fatal, which exits the process rather than
    // throwing, so this is only observable in a child.
    const ChildRun run = parseInChild({"ktplace"});
    BOOST_TEST(WIFEXITED(run.status));
    BOOST_TEST(WEXITSTATUS(run.status) == EXIT_FAILURE);
    BOOST_TEST(run.output.find("Insufficient command-line arguments") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(help_and_version_stop_before_anything_else) {
    // Help has to win over a missing argument list, otherwise `ktplace --help`
    // would be an error instead of an answer.
    for (const char *flag : {"-h", "--help", "-V", "--version"}) {
        std::vector<std::string> copy{"ktplace", flag};
        std::vector<char *> argv = getArgvOf(copy);
        const ChildRun run = runInChild([&argv] {
            kt_option opt;
            const bool ready = opt.parse(static_cast<int>(argv.size()), argv.data());
            // A parser that reported Ready here would be claiming there is
            // something to run.
            std::fflush(nullptr);
            ::_exit(ready ? EXIT_FAILURE : EXIT_SUCCESS);
        });
        BOOST_TEST_CONTEXT(flag) {
            BOOST_TEST(WIFEXITED(run.status));
            BOOST_TEST(WEXITSTATUS(run.status) == EXIT_SUCCESS);
            BOOST_TEST(!run.output.empty());
        }
    }
    BOOST_TEST(runInChild([] {
                   std::vector<std::string> a{"ktplace", "--help"};
                   std::vector<char *> v;
                   for (std::string &s : a) {
                       v.push_back(s.data());
                   }
                   kt_option opt;
                   opt.parse(static_cast<int>(v.size()), v.data());
               }).output.find("Usage:") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(parsing_twice_uses_the_second_command_line) {
    kt_option opt;
    std::vector<std::string> first{"ktplace", design};
    std::vector<char *> fargv = getArgvOf(first);
    BOOST_TEST(opt.parse(static_cast<int>(fargv.size()), fargv.data()));

    std::vector<std::string> second{"ktplace", design, "-v", "-tcl", "run.tcl"};
    std::vector<char *> sargv = getArgvOf(second);
    BOOST_TEST(opt.parse(static_cast<int>(sargv.size()), sargv.data()));
    BOOST_TEST(opt.verbose);
    BOOST_TEST(opt.tclFile == "run.tcl");
}
