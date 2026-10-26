// @file test_flow.cc// End-to-end tests for the load -> place -> write flow// These drive FlowMgr exactly the way main() does, on tiny synthetic designs,// so the whole pipeline is covered without any benchmark data on disk.


#define BOOST_TEST_MODULE ktplace_flow
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
// Death tests verify the ktlog::fatal paths, which end the process on purpose.
#include "kt_batch.h"
#include "kt_flowMgr.h"
#include "kt_option.h"
#include "kt_shell.h"

#include "datamodel/kt_dm.h"
#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tcl.h"
#include "util/kt_history.h"

#include <algorithm>
#include <boost/test/unit_test_suite.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace ktplace;
namespace fs = std::filesystem;

struct IsolatedHome {
    IsolatedHome() {
        const std::filesystem::path home =
            std::filesystem::temp_directory_path() / ("ktplace_home_" + std::to_string(getpid()));
        std::filesystem::create_directories(home);
        setenv("HOME", home.c_str(), 1);
    }
    ~IsolatedHome() {
        std::error_code ec;
        std::filesystem::remove_all(std::getenv("HOME"), ec);
    }
};
BOOST_TEST_GLOBAL_FIXTURE(IsolatedHome);

namespace {

class ScratchDir {
public:
    explicit ScratchDir(const std::string &tag) {
        static int counter = 0;
        path =
            fs::temp_directory_path() / ("ktplace_flow_" + tag + "_" + std::to_string(++counter));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::string getStr() const {
        return path.string();
    }
    [[nodiscard]] fs::path getFile(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        std::filesystem::create_directories(getFile(name).parent_path());
        std::ofstream out(getFile(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }


private:
    fs::path path;
};

/// Four movable cells, a pad on each side, and two nets, so the solver has
/// real work to do on a die that is large enough to spread into.
/// The design lives in its own directory, because that directory's name is what
/// the reader takes the design name from.
std::filesystem::path getDesignDir(const ScratchDir &dir) {
    return dir.getFile("tiny");
}

/// The options a run under test gets: a design to read and a work directory to
/// write into. The output and plot paths are derived from the latter, so tests
/// read them back off the options rather than naming them twice.
kt_option getOptionsFor(const ScratchDir &dir, const std::string &inputPath) {
    kt_option opt;
    opt.inputPath = inputPath;
    opt.workDir = dir.getStr();
    return opt;
}

void writeBookshelfDesign(const ScratchDir &dir) {
    std::string nodes = "UCLA nodes 1.0\n\nNumNodes : 6\nNumTerminals : 2\n\n";
    for (int i = 0; i < 4; ++i) {
        nodes += "\tc" + std::to_string(i) + "\t1.0\t2.0\n";
    }
    nodes += "\tpadA\t1.0\t2.0\tterminal\n";
    nodes += "\tpadB\t1.0\t2.0\tterminal\n";
    dir.write("tiny/tiny.nodes", nodes);

    std::string nets = "UCLA nets 1.0\n\nNumNets : 2\nNumPins : 8\n\n";
    nets += "NetDegree : 4\tn0\n";
    nets += "\tpadA I\t: 0.0\t0.0\n";
    for (int i = 0; i < 3; ++i) {
        nets += "\tc" + std::to_string(i) + " O\t: 0.0\t0.0\n";
    }
    nets += "NetDegree : 4\tn1\n";
    nets += "\tc3 I\t: 0.0\t0.0\n";
    nets += "\tpadB O\t: 0.0\t0.0\n";
    nets += "\tc0 I\t: 0.0\t0.0\n";
    nets += "\tc1 O\t: 0.0\t0.0\n";
    dir.write("tiny/tiny.nets", nets);

    std::string pl = "UCLA pl 1.0\n\n";
    for (int i = 0; i < 4; ++i) {
        pl += "c" + std::to_string(i) + "\t" + std::to_string(i * 2) + "\t0 : N\n";
    }
    pl += "padA\t0\t0 : N /FIXED\n";
    pl += "padB\t8\t0 : N /FIXED\n";
    dir.write("tiny/tiny.pl", pl);

    dir.write("tiny/tiny.scl", R"(UCLA scl 1.0

NumRows : 1

CoreRow Horizontal
  Coordinate   :  0
  Height       :  2
  Sitewidth    :  1
  Sitespacing  :  1
  SubrowOrigin :  0 NumSites :  20
End
)");
}

/// Run `fn` in a child process and require that it terminates the process with
/// EXIT_FAILURE after printing `needle` on stderr.
///
/// Boost.Test 1.83 ships no death-test macros. The child re-runs this test
/// binary on the current case alone rather than calling `fn` straight after
/// fork(2): the parent has oneTBB worker threads by then, and a forked copy of
/// a threaded process cannot use them on macOS.
bool isFatalChild() {
    const auto &suite = boost::unit_test::framework::master_test_suite();
    for (int i = 1; i < suite.argc; ++i) {
        if (std::string(suite.argv[i]) == "--fatal-child") {
            return true;
        }
    }
    return false;
}

void expectFatalExit(const std::function<void()> &fn, const std::string &needle) {
    if (isFatalChild()) {
        fn();
        std::fflush(nullptr);
        ::_exit(EXIT_SUCCESS);
    }
    const std::string self = boost::unit_test::framework::master_test_suite().argv[0];
    const std::string only =
        "--run_test=" + boost::unit_test::framework::current_test_case().p_name.get();

    int channel[2];
    BOOST_REQUIRE_EQUAL(::pipe(channel), 0);

    const pid_t pid = ::fork();
    BOOST_REQUIRE(pid >= 0);
    if (pid == 0) {
        ::close(channel[0]);
        ::dup2(channel[1], STDERR_FILENO);
        ::close(channel[1]);
        std::FILE *devNull = std::fopen("/dev/null", "w");
        if (devNull != nullptr) {
            ::dup2(::fileno(devNull), STDOUT_FILENO);
        }
        ::execl(self.c_str(), self.c_str(), only.c_str(), "--", "--fatal-child",
                static_cast<char *>(nullptr));
        ::_exit(127);
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
    BOOST_CHECK(WIFEXITED(status));
    BOOST_CHECK_EQUAL(WEXITSTATUS(status), EXIT_FAILURE);
    BOOST_CHECK_MESSAGE(captured.find(needle) != std::string::npos,
                        "stderr did not contain \"" << needle << "\"; got: " << captured);
}

std::size_t countNonEmptyLines(const fs::path &file) {
    std::ifstream in(file);
    std::size_t count = 0;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// One run, three properties of the one file it produces. These were three tests
// and three runs of the same design, which is most of this suite's runtime.
BOOST_AUTO_TEST_CASE(flow_places_a_bookshelf_design_and_writes_pl) {
    const ScratchDir dir("place");
    writeBookshelfDesign(dir);
    const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());

    FlowMgr flow;
    flow.run(opt);

    BOOST_REQUIRE(fs::exists(opt.getOutputPath()));
    // One record per cell vertex, terminals included.
    BOOST_TEST(countNonEmptyLines(opt.getOutputPath()) == 6);

    std::vector<std::string> lines;
    double c0x = 0.0;
    double c0y = 0.0;
    std::ifstream placed(opt.getOutputPath());
    for (std::string line; std::getline(placed, line);) {
        lines.push_back(line);
        std::istringstream fields(line);
        std::string name;
        double x = 0.0;
        double y = 0.0;
        fields >> name >> x >> y;
        if (name == "c0") {
            c0x = x;
            c0y = y;
        }
    }

    int fixedPads = 0;
    int fixedCells = 0;
    for (const std::string &line : lines) {
        const bool isFixed = line.find("/FIXED") != std::string::npos;
        if (line.starts_with("padA\t") || line.starts_with("padB\t")) {
            fixedPads += isFixed;
        } else if (isFixed) {
            ++fixedCells;
        }
    }
    BOOST_TEST(fixedPads == 2);
    BOOST_TEST(fixedCells == 0);

    // The Bookshelf seed is degenerate (every cell at the origin), so a movable
    // cell has to have been given a real coordinate. c0 is seeded exactly where
    // padA is and fixed cells are blockages, so it must also end up clear of padA
    // (x in [0,1]); a non-zero y instead would be arbitrary, since sliding right
    // off a pad legitimately leaves y at 0.
    const bool movedFromSeed = c0x != 0.0 || c0y != 0.0;
    BOOST_TEST(movedFromSeed);
    BOOST_TEST(c0x >= 1.0);
}

BOOST_AUTO_TEST_CASE(flow_writes_visualization_output_when_asked) {
    const ScratchDir dir("plots");
    writeBookshelfDesign(dir);
    OptionDb::getInstance().set(Component::Visualizer, "svg_frames", OptionValue{1});
    const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
    const fs::path plots = opt.getPlotDir();

    {
        FlowMgr flow;
        flow.run(opt);
    }
    OptionDb::getInstance().clear();

    // SimPL plots the LSS/HPWL bound it converges against, not the raw
    // per-iteration HPWL series the quadratic placer emitted.
    BOOST_REQUIRE(fs::exists(plots / "simpl_convergence.png"));

    std::size_t stills = 0;
    // SimPL's default stills are SVG (simpl_*.svg) under <plotDir>/simpl, nested
    // so they do not collide with the bounds curve. The raster
    // frame_*.ppm stills only appear when a CG trace cadence is requested.
    for (const auto &entry : fs::directory_iterator(plots / "simpl")) {
        const std::string name = entry.path().filename().string();
        if (name.starts_with("simpl_") && entry.path().extension() == ".svg") {
            ++stills;
        }
    }
    BOOST_TEST(stills > 0);
}

BOOST_AUTO_TEST_CASE(flow_handles_a_lefdef_design) {
    const ScratchDir dir("lefdef");
    dir.write("tech.lef", R"(VERSION 5.8 ;
UNITS
  DATABASE MICRONS 1000 ;
END UNITS
SITE core
  CLASS CORE ;
  SIZE 0.100 BY 0.900 ;
END core
END LIBRARY
)");
    dir.write("cells.lef", R"(VERSION 5.8 ;
MACRO NSTD
  CLASS CORE ;
  SIZE 0.200 BY 0.900 ;
  PIN A
    DIRECTION INPUT ;
    PORT
      RECT 0.000 0.000 0.050 0.100 ;
    END
  END A
END NSTD
MACRO MACRO1
  CLASS BLOCK ;
  SIZE 10.000 BY 20.000 ;
  PIN A
    DIRECTION INPUT ;
    PORT
      RECT 0.000 0.000 0.100 0.100 ;
    END
  END A
END MACRO1
END LIBRARY
)");
    dir.write("floorplan.def", R"(VERSION 5.8 ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 100000 100000 ) ;
ROW row0 core 0 0 N DO 100 BY 100 STEP 100 900 ;
COMPONENTS 3 ;
	- u1 NSTD + PLACED ( 10000 20000 ) N ;
	- u2 NSTD + PLACED ( 12000 20000 ) N ;
	- m1 MACRO1 + FIXED ( 50000 50000 ) N ;
END COMPONENTS
PINS 1 ;
	- p1 + NET n0 + PLACED ( 0 0 ) N ;
END PINS
NETS 2 ;
	- n0 ( u1 A ) ( u2 A ) ( PIN p1 ) ;
	- n1 ( u1 A ) ( m1 A ) ;
END NETS
END DESIGN
)");
    const kt_option opt = getOptionsFor(dir, dir.getStr());

    FlowMgr flow;
    // Format auto-detection picks LEF/DEF because the directory holds a .def.
    flow.run(opt);

    BOOST_REQUIRE(fs::exists(opt.getOutputPath()));
    BOOST_TEST(countNonEmptyLines(opt.getOutputPath()) == 4);  // 3 components + 1 pad
}

BOOST_AUTO_TEST_CASE(flow_exits_on_an_unknown_algorithm) {
    const ScratchDir dir("badalgo");
    writeBookshelfDesign(dir);
    expectFatalExit(
        [&] {
            FlowMgr flow;
            OptionDb::getInstance().set(Component::Flow, "algorithm", std::string("no_such_algo"));
            const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
            flow.run(opt);
        },
        "unknown placer");
}

BOOST_AUTO_TEST_CASE(flow_reads_the_placer_from_its_tcl_script) {
    const ScratchDir dir("tclalgo");
    writeBookshelfDesign(dir);
    const fs::path script = fs::path(dir.getStr()) / "run.tcl";
    std::ofstream(script) << "add_option flow algorithm no_such_algo\n";
    expectFatalExit(
        [&] {
            FlowMgr flow;
            kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
            opt.tclFile = script.string();
            flow.run(opt);
        },
        "unknown placer 'no_such_algo'");
}

BOOST_AUTO_TEST_CASE(a_script_with_steps_is_the_flow) {
    const ScratchDir dir("stepscript");
    writeBookshelfDesign(dir);
    const fs::path script = fs::path(dir.getStr()) / "flow.tcl";
    std::ofstream(script) << "add_option visualizer plot 0\n"
                          << "read_design {" << getDesignDir(dir).string() << "}\n"
                          << "gplace_design\n"
                          << "set gp [report_hpwl]\n"
                          << "legalize_placement\n"
                          << "detail_place\n"
                          << "write_placement {" << dir.getStr() << "/mine.pl}\n";
    kt_option opt = getOptionsFor(dir, "");
    opt.tclFile = script.string();
    {
        FlowMgr flow;
        flow.run(opt);
    }
    OptionDb::getInstance().clear();
    BOOST_TEST(fs::exists(fs::path(dir.getStr()) / "mine.pl"));
    BOOST_TEST(!fs::exists(opt.getOutputPath()));
    BOOST_TEST(countNonEmptyLines(fs::path(dir.getStr()) / "mine.pl") == 6);
}

BOOST_AUTO_TEST_CASE(a_step_before_read_design_is_a_tcl_error) {
    const ScratchDir dir("nodesign");
    FlowMgr flow;
    flow.begin(getOptionsFor(dir, ""));
    BOOST_CHECK_THROW((void)flow.evaluate("gplace_design"), tcl::Error);
    BOOST_CHECK_THROW((void)flow.evaluate("write_placement"), tcl::Error);
    BOOST_CHECK_THROW((void)flow.evaluate("read_design"), tcl::Error);
    BOOST_TEST(!flow.hasDesign());
}

BOOST_AUTO_TEST_CASE(report_hpwl_returns_the_hpwl_to_the_script) {
    const ScratchDir dir("hpwl");
    writeBookshelfDesign(dir);
    FlowMgr flow;
    flow.begin(getOptionsFor(dir, ""));
    (void)flow.evaluate("read_design {" + getDesignDir(dir).string() + "}");
    BOOST_TEST(flow.hasDesign());
    BOOST_TEST(flow.evaluate("read_design") == getDesignDir(dir).string());
    BOOST_TEST(std::stod(flow.evaluate("report_hpwl")) > 0.0);
}

BOOST_AUTO_TEST_CASE(help_lists_every_command_and_explains_one) {
    const ScratchDir dir("help");
    FlowMgr flow;
    flow.begin(getOptionsFor(dir, ""));
    const std::string all = flow.evaluate("help");
    for (const char *command : {"read_design", "gplace_design", "legalize_placement",
                                "detail_place", "place_design", "write_placement", "report_hpwl"}) {
        BOOST_TEST(all.find(command) != std::string::npos, command);
    }
    BOOST_TEST(flow.evaluate("help write_placement").find("write_placement") != std::string::npos);
    BOOST_CHECK_THROW((void)flow.evaluate("help no_such_command"), tcl::Error);
    BOOST_TEST(all.find("foreach") > all.find("Tcl builtins:"));
    BOOST_TEST(flow.evaluate("place_design -h") == flow.evaluate("help place_design"));
    BOOST_TEST(flow.evaluate("gplace_design -h").starts_with("gplace_design [simpl|ntuplace1]"));
    BOOST_CHECK_THROW((void)flow.evaluate("read_design a b c"), tcl::Error);
}

BOOST_AUTO_TEST_CASE(the_shell_remembers_its_commands) {
    const ScratchDir dir("shellhistory");
    std::istringstream input("help\nreport_design\nexit\n");
    BOOST_TEST(runShell(getOptionsFor(dir, ""), input) == 0);
    const CommandHistory history;
    BOOST_TEST(std::ranges::count(history.getEntries(), "help") == 1);
}

BOOST_AUTO_TEST_CASE(running_a_freshly_loaded_design_does_not_read_it_again) {
    const ScratchDir dir("freshrun");
    writeBookshelfDesign(dir);
    OptionDb::getInstance().set(Component::Visualizer, "plot", OptionValue{0});
    const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
    const auto reads = [&opt] {
        std::ifstream log(opt.getLogFile());
        std::size_t count = 0;
        for (std::string line; std::getline(log, line);) {
            count += line.find("Reading bookshelf input") != std::string::npos ? 1 : 0;
        }
        return count;
    };
    FlowMgr flow;
    flow.begin(opt);
    flow.readDesign(opt.inputPath);
    BOOST_TEST(flow.isFreshlyLoaded(opt.inputPath));
    flow.run(opt);
    BOOST_TEST(reads() == 1U);
    BOOST_TEST(!flow.isFreshlyLoaded(opt.inputPath));
    BOOST_TEST(fs::exists(opt.getOutputPath()));
    flow.run(opt);
    BOOST_TEST(reads() == 2U);
    OptionDb::getInstance().clear();
}

BOOST_AUTO_TEST_CASE(a_batch_run_places_the_design_and_succeeds) {
    const ScratchDir dir("batch");
    writeBookshelfDesign(dir);
    OptionDb::getInstance().set(Component::Visualizer, "plot", OptionValue{0});
    const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
    BOOST_TEST(runBatch(opt) == 0);
    OptionDb::getInstance().clear();
    BOOST_TEST(fs::exists(opt.getOutputPath()));
}

BOOST_AUTO_TEST_CASE(the_shell_exits_with_the_status_it_is_given) {
    const ScratchDir dir("shellexit");
    std::istringstream input("help\nexit 3\nhelp\n");
    BOOST_TEST(runShell(getOptionsFor(dir, ""), input) == 3);
}

BOOST_AUTO_TEST_CASE(the_shell_fails_when_its_startup_script_fails) {
    const ScratchDir dir("shellbadscript");
    const fs::path script = dir.getFile("broken.tcl");
    std::ofstream(script) << "no_such_command\n";
    kt_option opt = getOptionsFor(dir, "");
    opt.tclFile = script.string();
    std::istringstream input("");
    BOOST_TEST(runShell(opt, input) == 1);
}

BOOST_AUTO_TEST_CASE(the_shell_runs_commands_and_survives_a_bad_one) {
    const ScratchDir dir("shell");
    writeBookshelfDesign(dir);
    std::istringstream input(
        "add_option visualizer plot 0\n"
        "place_design\n"
        "no_such_command\n"
        "foreach step {1 2} {\n"
        "  set last $step\n"
        "}\n"
        "write_placement\n"
        "exit\n");
    const kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
    BOOST_TEST(runShell(opt, input) == 0);
    OptionDb::getInstance().clear();
    BOOST_TEST(fs::exists(opt.getOutputPath()));
}

BOOST_AUTO_TEST_CASE(flow_exits_on_a_broken_tcl_script) {
    const ScratchDir dir("badtcl");
    writeBookshelfDesign(dir);
    const fs::path script = fs::path(dir.getStr()) / "run.tcl";
    std::ofstream(script) << "add_option nonesuch grid 64\n";
    expectFatalExit(
        [&] {
            FlowMgr flow;
            kt_option opt = getOptionsFor(dir, getDesignDir(dir).string());
            opt.tclFile = script.string();
            flow.run(opt);
        },
        "is not a known component");
}

BOOST_AUTO_TEST_CASE(flow_exits_when_the_input_directory_is_missing) {
    const ScratchDir dir("badinput");
    expectFatalExit(
        [&] {
            FlowMgr flow;
            flow.run(getOptionsFor(dir, dir.getStr() + "/no_such_dir"));
        },
        "cannot read input directory");
}

BOOST_AUTO_TEST_CASE(flow_exits_when_the_design_files_are_missing) {
    const ScratchDir dir("nofiles");
    // The directory exists but holds no .nodes, which must not be mistaken for
    // an empty design.
    expectFatalExit(
        [&] {
            FlowMgr flow;
            flow.run(getOptionsFor(dir, dir.getStr()));
        },
        "no input format recognises");
}

BOOST_AUTO_TEST_CASE(a_run_draws_its_recorded_pictures_and_removes_the_snapshot_file) {
    const ScratchDir dir("drawn");
    writeBookshelfDesign(dir);
    const kt_option options = getOptionsFor(dir, getDesignDir(dir).string());
    {
        FlowMgr flow;
        flow.run(options);
    }
    const fs::path plots = options.getPlotDir();
    BOOST_TEST(fs::exists(plots / "placement.png"));
    BOOST_TEST(fs::exists(plots / "placement.gif"));
    BOOST_TEST(!fs::exists(plots / "frames"));
    BOOST_TEST(!fs::exists(plots / "simpl"));
    BOOST_TEST(!fs::exists(plots / "snapshots.ktsnap"));
}

BOOST_AUTO_TEST_CASE(keep_snapshots_leaves_the_snapshot_file) {
    const ScratchDir dir("kept");
    writeBookshelfDesign(dir);
    OptionDb::getInstance().set(Component::Visualizer, "keep_snapshots", OptionValue{1});
    const kt_option options = getOptionsFor(dir, getDesignDir(dir).string());
    {
        FlowMgr flow;
        flow.run(options);
    }
    OptionDb::getInstance().clear();
    BOOST_TEST(fs::exists(fs::path(options.getPlotDir()) / "snapshots.ktsnap"));
}

BOOST_AUTO_TEST_CASE(an_observer_sees_every_commit_and_the_stage_frames_without_plots) {
    const ScratchDir dir("observed");
    writeBookshelfDesign(dir);
    const kt_option options = getOptionsFor(dir, getDesignDir(dir).string());
    OptionDb::getInstance().set(Component::Visualizer, "plot", OptionValue{0});

    std::vector<std::string> stages;
    std::size_t cells = 0;
    std::size_t frames = 0;
    std::size_t frameSize = 0;
    FlowMgr flow;
    flow.observe({.committed =
                      [&](std::string_view stage, const ktDM &db) {
                          stages.emplace_back(stage);
                          cells = db.getNumCells();
                      },
                  .frame =
                      [&](std::string_view, const std::vector<float> &x,
                          const std::vector<float> &y, double) {
                          ++frames;
                          frameSize = std::min(x.size(), y.size());
                      }});
    flow.run(options);
    OptionDb::getInstance().clear();

    const std::vector<std::string> expected{"load", "global placement", "legalization",
                                            "detailed placement"};
    BOOST_TEST(stages == expected, boost::test_tools::per_element());
    BOOST_TEST(cells == 6U);
    BOOST_TEST(frames > 0U);
    BOOST_TEST(frameSize == 6U);
    BOOST_TEST(fs::exists(options.getOutputPath()));
    BOOST_TEST(!fs::exists(dir.getFile("plots")));
}

BOOST_AUTO_TEST_CASE(an_observer_that_throws_stops_the_run_before_it_writes) {
    const ScratchDir dir("stopped");
    writeBookshelfDesign(dir);
    const kt_option options = getOptionsFor(dir, getDesignDir(dir).string());
    OptionDb::getInstance().set(Component::Visualizer, "plot", OptionValue{0});

    struct Stop {};
    FlowMgr flow;
    flow.observe({.committed = {},
                  .frame = [](std::string_view, const std::vector<float> &,
                              const std::vector<float> &, double) {
                      throw Stop{};
                  }});
    BOOST_CHECK_THROW(flow.run(options), Stop);
    OptionDb::getInstance().clear();
    BOOST_TEST(!fs::exists(options.getOutputPath()));
}
