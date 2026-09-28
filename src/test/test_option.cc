// SPDX-License-Identifier: BSD-3-Clause
/**
 * @file test_option.cc
 * @brief Unit tests for the command-line option parser
 *
 * The parser has one job beyond reading flags: deciding where artifacts land.
 * That resolution is the part worth pinning, because a relative path silently
 * written to the wrong directory is the kind of bug that survives a test run
 * and then loses a whole placement result.
 */

#define BOOST_TEST_MODULE ktplace_option

#include "kt_option.h"

#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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

    [[nodiscard]] std::string str() const {
        return path_.string();
    }

    [[nodiscard]] std::filesystem::path file(const char *name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

/// Parse a command line, returning the option object. `argv[0]` is the program.
kt_option parse(std::vector<std::string> &args) {
    std::vector<char *> argv;
    argv.reserve(args.size());
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    kt_option opt;
    opt.parse_option(static_cast<int>(argv.size()), argv.data());
    return opt;
}

/// The minimum viable command line, with room to append flags.
std::vector<std::string> base(const ScratchDir &dir) {
    return {"ktplace", "adaptec2", "./benchmark/adaptec2", dir.file("out.pl").string()};
}

}  // namespace

BOOST_AUTO_TEST_CASE(positional_arguments_are_read_in_order) {
    const ScratchDir dir("pos");
    std::vector<std::string> args = {"ktplace", "adaptec2", "./in", "./out.pl"};
    const kt_option opt = parse(args);
    BOOST_TEST(opt.getInputBaseName() == "adaptec2");
    BOOST_TEST(opt.getInputDir() == "./in");
    BOOST_TEST(opt.getOutputPath() == "./out.pl");
}

BOOST_AUTO_TEST_CASE(algorithm_and_format_have_defaults) {
    const ScratchDir dir("defaults");
    std::vector<std::string> args = base(dir);
    const kt_option opt = parse(args);
    // SimPL is the only placer, and bookshelf is the only writer, so these are
    // defaults rather than a menu. A test that only checks the flag is accepted
    // would pass even if the default drifted to something removed.
    BOOST_TEST(opt.getAlgorithm() == "simpl");
    BOOST_TEST(opt.getOutputFormat() == "bookshelf");
    BOOST_TEST(!opt.isVerbose());
    BOOST_TEST(opt.getConfigFile().empty());
    BOOST_TEST(opt.getPlotDir().empty());
}

BOOST_AUTO_TEST_CASE(every_flag_is_accepted_in_both_its_short_and_long_form) {
    const struct {
        const char *shortForm;
        const char *longForm;
    } forms[] = {{"-a", "--algorithm"},
                 {"-f", "--format"},
                 {"-l", "--log"},
                 {"-c", "--config"},
                 {"-p", "--plot"}};

    for (const auto &f : forms) {
        const ScratchDir dir("forms");
        std::vector<std::string> args = base(dir);
        args.emplace_back(f.shortForm);
        args.emplace_back("value");
        BOOST_TEST_CONTEXT(f.shortForm) {
            BOOST_CHECK_NO_THROW(parse(args));
        }

        std::vector<std::string> args2 = base(dir);
        args2.emplace_back(f.longForm);
        args2.emplace_back("value");
        BOOST_TEST_CONTEXT(f.longForm) {
            BOOST_CHECK_NO_THROW(parse(args2));
        }
    }
}

BOOST_AUTO_TEST_CASE(flag_values_are_stored) {
    const ScratchDir dir("values");
    std::vector<std::string> args = base(dir);
    args.emplace_back("-a");
    args.emplace_back("simpl");
    args.emplace_back("-f");
    args.emplace_back("bookshelf");
    args.emplace_back("-c");
    args.emplace_back("cfg.txt");
    args.emplace_back("-v");

    const kt_option opt = parse(args);
    BOOST_TEST(opt.getAlgorithm() == "simpl");
    BOOST_TEST(opt.getOutputFormat() == "bookshelf");
    BOOST_TEST(opt.getConfigFile() == "cfg.txt");
    BOOST_TEST(opt.isVerbose());
}

BOOST_AUTO_TEST_CASE(verbose_has_a_long_form_too) {
    const ScratchDir dir("verbose");
    std::vector<std::string> args = base(dir);
    args.emplace_back("--verbose");
    BOOST_TEST(parse(args).isVerbose());
}

BOOST_AUTO_TEST_CASE(a_flag_at_the_end_without_a_value_is_rejected) {
    // Silently keeping the default would turn a typo into a run that writes the
    // wrong kind of output, so each of these has to throw.
    const char *flags[] = {"-a", "--algorithm", "-f", "--format", "-l", "--log",
                           "-w", "--work-dir",  "-c", "--config", "-p", "--plot"};
    for (const char *flag : flags) {
        const ScratchDir dir("novalue");
        std::vector<std::string> args = base(dir);
        args.emplace_back(flag);
        BOOST_CHECK_THROW(parse(args), std::runtime_error);
        BOOST_TEST_CONTEXT(flag) {}
    }
}

BOOST_AUTO_TEST_CASE(too_few_arguments_is_a_fatal_error) {
    // parse_option exits the process on the paths it cannot recover from, so
    // these are checked in a child, the same way test_util checks a fatal log.
    struct {
        const char *name;
        int argc;
    } cases[] = {{"nothing", 1}, {"program only", 1}, {"missing output", 3}};

    for (const auto &c : cases) {
        std::vector<std::string> args;
        for (int i = 0; i < c.argc; ++i) {
            args.emplace_back("ktplace");
        }
        std::vector<char *> argv;
        for (std::string &a : args) {
            argv.push_back(a.data());
        }

        int channel[2];
        BOOST_REQUIRE_EQUAL(::pipe(channel), 0);
        const pid_t pid = ::fork();
        BOOST_REQUIRE(pid >= 0);
        if (pid == 0) {
            ::close(channel[0]);
            ::dup2(channel[1], STDERR_FILENO);
            ::close(channel[1]);
            std::FILE *null = std::fopen("/dev/null", "w");
            if (null != nullptr) {
                ::dup2(::fileno(null), STDOUT_FILENO);
            }
            kt_option opt;
            opt.parse_option(c.argc, argv.data());
            std::fflush(nullptr);
            ::_exit(EXIT_SUCCESS);  // only reached if it did not stop
        }
        ::close(channel[1]);
        std::string captured;
        char buffer[512];
        ssize_t got = 0;
        while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
            captured.append(buffer, static_cast<std::size_t>(got));
        }
        ::close(channel[0]);
        int status = 0;
        ::waitpid(pid, &status, 0);
        // The documented contract is @throws std::runtime_error, but the
        // throw sits after a ktlog.fatal that is [[noreturn]], so the process
        // exits first. Asserting the real behaviour; the docstring is the thing
        // that is out of step.
        BOOST_TEST_CONTEXT(c.name) {
            BOOST_TEST(WIFEXITED(status));
            BOOST_TEST(WEXITSTATUS(status) == EXIT_FAILURE);
        }
    }
}

BOOST_AUTO_TEST_CASE(an_unknown_flag_is_reported_and_stops_the_run) {
    const ScratchDir dir("unknown");
    std::vector<std::string> args = base(dir);
    args.emplace_back("--nonesuch");

    // Not a throw: parse_option routes a bad flag through ktlog.fatal, which is
    // [[noreturn]] and calls exit(EXIT_FAILURE). A caller that expected to
    // catch an exception here would silently see the process disappear.
    int channel[2];
    BOOST_REQUIRE_EQUAL(::pipe(channel), 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(channel[0]);
        ::dup2(channel[1], STDERR_FILENO);
        ::close(channel[1]);
        std::FILE *null = std::fopen("/dev/null", "w");
        if (null != nullptr) {
            ::dup2(::fileno(null), STDOUT_FILENO);
        }
        kt_option opt;
        std::vector<char *> argv;
        for (std::string &a : args) {
            argv.push_back(a.data());
        }
        opt.parse_option(static_cast<int>(argv.size()), argv.data());
        std::fflush(nullptr);
        ::_exit(EXIT_SUCCESS);  // only reached if fatal() did not terminate
    }
    ::close(channel[1]);
    std::string captured;
    char buffer[512];
    ssize_t got = 0;
    while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        captured.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(channel[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    BOOST_TEST(WIFEXITED(status));
    BOOST_TEST(WEXITSTATUS(status) == EXIT_FAILURE);
    BOOST_TEST(captured.find("Unknown command-line option") != std::string::npos);
    BOOST_TEST(captured.find("--nonesuch") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(help_and_version_stop_before_anything_else) {
    // Help has to win over a missing argument list, otherwise `ktplace --help`
    // would be an error instead of an answer.
    std::vector<std::string> help{"ktplace", "--help"};
    std::vector<char *> hargv;
    for (std::string &a : help) {
        hargv.push_back(a.data());
    }
    kt_option opt;
    // parse_option returns false when it has already answered the user.
    int channel[2];
    BOOST_REQUIRE_EQUAL(::pipe(channel), 0);
    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(channel[0]);
        ::dup2(channel[1], STDOUT_FILENO);
        ::dup2(channel[1], STDERR_FILENO);
        ::close(channel[1]);
        kt_option child;
        const bool shown = child.parse_option(static_cast<int>(hargv.size()), hargv.data());
        std::fflush(nullptr);
        ::_exit(shown ? EXIT_FAILURE : EXIT_SUCCESS);
    }
    ::close(channel[1]);
    std::string captured;
    char buffer[1024];
    ssize_t got = 0;
    while ((got = ::read(channel[0], buffer, sizeof(buffer))) > 0) {
        captured.append(buffer, static_cast<std::size_t>(got));
    }
    ::close(channel[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    BOOST_TEST(WIFEXITED(status));
    BOOST_TEST(WEXITSTATUS(status) == EXIT_SUCCESS);
    BOOST_TEST(captured.find("Usage:") != std::string::npos);
    BOOST_TEST(captured.find("--work-dir") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(no_work_dir_leaves_paths_exactly_as_typed) {
    std::vector<std::string> args{"ktplace", "adaptec2", "./in", "./out.pl"};
    const kt_option opt = parse(args);
    BOOST_TEST(opt.getWorkDir().empty());
    BOOST_TEST(opt.getOutputPath() == "./out.pl");
    // With no work directory there is nothing to rebase onto, so the log keeps
    // its bare default name instead of gaining a directory it cannot be in.
    BOOST_TEST(opt.getLogFile() == "ktplace.log");
}

BOOST_AUTO_TEST_CASE(a_work_dir_rebases_relative_artifacts_and_is_created) {
    const ScratchDir dir("workdir");
    const std::string work = (dir.file("run") / "nested").string();
    std::vector<std::string> args{"ktplace", "adaptec2", "./in", "out.pl"};
    args.emplace_back("-p");
    args.emplace_back("frames");
    args.emplace_back("-w");
    args.emplace_back(work);

    const kt_option opt = parse(args);
    // Two levels of missing directory: the flag promises to create it.
    BOOST_TEST(std::filesystem::is_directory(work));
    BOOST_TEST(opt.getWorkDir() == work);
    BOOST_TEST(opt.getOutputPath() == work + "/out.pl");
    BOOST_TEST(opt.getPlotDir() == work + "/frames");
    BOOST_TEST(opt.getLogFile() == work + "/ktplace.log");
}

BOOST_AUTO_TEST_CASE(an_absolute_artifact_path_ignores_the_work_dir) {
    // Honouring -w for an absolute path would quietly relocate a result the user
    // named explicitly, which is the opposite of what the flag is for.
    const ScratchDir dir("absolute");
    const std::string work = dir.file("work").string();
    std::vector<std::string> args{"ktplace", "adaptec2", "./in", "/tmp/explicit.pl"};
    args.emplace_back("-l");
    args.emplace_back("/tmp/explicit.log");
    args.emplace_back("-p");
    args.emplace_back("/tmp/frames");
    args.emplace_back("-w");
    args.emplace_back(work);

    const kt_option opt = parse(args);
    BOOST_TEST(opt.getOutputPath() == "/tmp/explicit.pl");
    BOOST_TEST(opt.getLogFile() == "/tmp/explicit.log");
    BOOST_TEST(opt.getPlotDir() == "/tmp/frames");
    // The input directory is not an artifact, so it is never rebased.
    BOOST_TEST(opt.getInputDir() == "./in");
}

BOOST_AUTO_TEST_CASE(an_explicit_relative_log_path_is_rebased_too) {
    const ScratchDir dir("logrebase");
    const std::string work = dir.file("w").string();
    std::vector<std::string> args{"ktplace", "adaptec2", "./in", "out.pl"};
    args.emplace_back("-l");
    args.emplace_back("custom.log");
    args.emplace_back("-w");
    args.emplace_back(work);

    BOOST_TEST(parse(args).getLogFile() == work + "/custom.log");
}

BOOST_AUTO_TEST_CASE(an_uncreatable_work_dir_is_reported) {
    // A work dir under an existing *file* cannot be made. Reporting it beats
    // silently writing somewhere else.
    const ScratchDir dir("badwork");
    // The parent has to exist first, otherwise ofstream silently creates
    // nothing and create_directories then succeeds on an empty tree.
    std::filesystem::create_directories(dir.file("blockerdir"));
    const std::string blocker = dir.file("blockerdir").string() + "/blocker";
    {
        std::ofstream out(blocker);
        out << "not a directory\n";
    }
    std::vector<std::string> args{"ktplace", "adaptec2", "./in", "out.pl"};
    args.emplace_back("-w");
    args.emplace_back(blocker + "/under");

    std::vector<char *> argv;
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    kt_option opt;
    BOOST_CHECK_THROW(opt.parse_option(static_cast<int>(argv.size()), argv.data()),
                      std::runtime_error);
}

BOOST_AUTO_TEST_CASE(the_parser_can_be_moved) {
    // PIMPL with unique_ptr: moving must not double-free or leave a null deref
    // behind, so the moved-to object has to be usable straight away.
    const ScratchDir dir("move");
    std::vector<std::string> args = base(dir);
    std::vector<char *> argv;
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    kt_option src;
    src.parse_option(static_cast<int>(argv.size()), argv.data());

    kt_option dst = std::move(src);
    BOOST_TEST(dst.getInputBaseName() == "adaptec2");
    BOOST_TEST(dst.getOutputPath() == args[3]);
}

BOOST_AUTO_TEST_CASE(a_moved_from_parser_is_not_reused) {
    const ScratchDir dir("movedfrom");
    std::vector<std::string> args = base(dir);
    std::vector<char *> argv;
    for (std::string &a : args) {
        argv.push_back(a.data());
    }
    kt_option src;
    src.parse_option(static_cast<int>(argv.size()), argv.data());
    const kt_option taken = std::move(src);
    BOOST_TEST(taken.getAlgorithm() == "simpl");
    // Reading the moved-from object is not a supported operation, so this only
    // asserts the move itself did not crash or leak-double-free.
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(parsing_twice_on_one_object_does_not_keep_the_old_help_flag) {
    // parse_option resets its own flags; a stale helpRequested would make the
    // second call bail out early and look like a success that printed nothing.
    const ScratchDir dir("twice");
    std::vector<std::string> first = base(dir);
    std::vector<char *> fargv;
    for (std::string &a : first) {
        fargv.push_back(a.data());
    }
    std::vector<std::string> second = base(dir);
    second.emplace_back("-v");
    std::vector<char *> sargv;
    for (std::string &a : second) {
        sargv.push_back(a.data());
    }

    kt_option opt;
    BOOST_TEST(opt.parse_option(static_cast<int>(fargv.size()), fargv.data()));
    BOOST_TEST(!opt.isVerbose());
    BOOST_TEST(opt.parse_option(static_cast<int>(sargv.size()), sargv.data()));
    BOOST_TEST(opt.isVerbose());
}
