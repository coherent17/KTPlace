/**
 * @file test_flow.cc
 * @brief End-to-end tests for the load -> place -> write flow
 *
 * These drive FlowMgr exactly the way main() does, on tiny synthetic designs,
 * so the whole pipeline is covered without any benchmark data on disk.
 */

#define BOOST_TEST_MODULE ktplace_flow
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
// Death tests verify the ktlog::fatal paths, which end the process on purpose.
#include <boost/test/unit_test_suite.hpp>

#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <functional>

#include "kt_flowMgr.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace ktplace;
namespace fs = std::filesystem;

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

    [[nodiscard]] std::string str() const {
        return path.string();
    }
    [[nodiscard]] fs::path file(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        std::ofstream out(file(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }

    [[nodiscard]] std::vector<std::string> readLines(const std::string &name) const {
        std::ifstream in(file(name));
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                lines.push_back(line);
            }
        }
        return lines;
    }

private:
    fs::path path;
};

/// Four movable cells, a pad on each side, and two nets, so the solver has
/// real work to do on a die that is large enough to spread into.
void writeBookshelfDesign(const ScratchDir &dir) {
    std::string nodes = "UCLA nodes 1.0\n\nNumNodes : 6\nNumTerminals : 2\n\n";
    for (int i = 0; i < 4; ++i) {
        nodes += "\tc" + std::to_string(i) + "\t1.0\t2.0\n";
    }
    nodes += "\tpadA\t1.0\t2.0\tterminal\n";
    nodes += "\tpadB\t1.0\t2.0\tterminal\n";
    dir.write("tiny.nodes", nodes);

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
    dir.write("tiny.nets", nets);

    std::string pl = "UCLA pl 1.0\n\n";
    for (int i = 0; i < 4; ++i) {
        pl += "c" + std::to_string(i) + "\t" + std::to_string(i * 2) + "\t0 : N\n";
    }
    pl += "padA\t0\t0 : N /FIXED\n";
    pl += "padB\t8\t0 : N /FIXED\n";
    dir.write("tiny.pl", pl);

    dir.write("tiny.scl", R"(UCLA scl 1.0

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

/// Run `fn` in a forked child and require that it terminates the process with
/// EXIT_FAILURE after printing `needle` on stderr.
///
/// Boost.Test 1.83 ships no death-test macros, so this is done with fork(2).
/// The child exits via _exit() so it never flushes the parent's test buffers.
void expectFatalExit(const std::function<void()> &fn, const std::string &needle) {
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
        fn();
        std::fflush(nullptr);
        ::_exit(EXIT_SUCCESS);  // only reached when the call did NOT terminate
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

BOOST_AUTO_TEST_CASE(flow_places_a_bookshelf_design_and_writes_pl) {
    const ScratchDir dir("place");
    writeBookshelfDesign(dir);
    const fs::path out = dir.file("result.pl");

    FlowMgr flow;
    flow.run("tiny", dir.str(), out.string());

    BOOST_REQUIRE(fs::exists(out));
    // One record per cell vertex, terminals included.
    BOOST_TEST(countNonEmptyLines(out) == 6);
}

BOOST_AUTO_TEST_CASE(flow_output_marks_pads_fixed_and_leaves_cells_movable) {
    const ScratchDir dir("fixed");
    writeBookshelfDesign(dir);
    const fs::path out = dir.file("result.pl");

    FlowMgr flow;
    flow.run("tiny", dir.str(), out.string());

    const auto lines = [&] {
        std::ifstream in(out);
        std::vector<std::string> result;
        for (std::string line; std::getline(in, line);) {
            result.push_back(line);
        }
        return result;
    }();

    int fixedPads = 0;
    int fixedCells = 0;
    for (const std::string &line : lines) {
        const bool isFixed = line.find("/FIXED") != std::string::npos;
        if (line.find("padA\t") == 0 || line.find("padB\t") == 0) {
            if (isFixed) {
                ++fixedPads;
            }
        } else if (isFixed) {
            ++fixedCells;
        }
    }
    BOOST_TEST(fixedPads == 2);
    BOOST_TEST(fixedCells == 0);
}

BOOST_AUTO_TEST_CASE(flow_writes_visualization_output_when_asked) {
    const ScratchDir dir("plots");
    writeBookshelfDesign(dir);
    const fs::path out = dir.file("result.pl");
    const fs::path plots = dir.file("plots");

    FlowMgr flow;
    flow.run("tiny", dir.str(), out.string(), "quadratic", "bookshelf", plots.string());

    BOOST_REQUIRE(fs::exists(plots / "index.html"));
    BOOST_REQUIRE(fs::exists(plots / "hpwl.csv"));
    BOOST_REQUIRE(fs::exists(plots / "hpwl.svg"));
    BOOST_TEST(countNonEmptyLines(plots / "hpwl.csv") > 0);

    std::size_t frames = 0;
    for (const auto &entry : fs::directory_iterator(plots)) {
        if (entry.path().filename().string().rfind("frame_step_", 0) == 0) {
            ++frames;
        }
    }
    BOOST_TEST(frames > 0);
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
    const fs::path out = dir.file("result.pl");

    FlowMgr flow;
    // Format auto-detection picks LEF/DEF because the directory holds a .def.
    flow.run("floorplan", dir.str(), out.string());

    BOOST_REQUIRE(fs::exists(out));
    BOOST_TEST(countNonEmptyLines(out) == 4);  // 3 components + 1 pad
}

BOOST_AUTO_TEST_CASE(flow_moves_movable_cells_off_the_seed_point) {
    const ScratchDir dir("spread");
    writeBookshelfDesign(dir);
    const fs::path out = dir.file("result.pl");

    FlowMgr flow;
    flow.run("tiny", dir.str(), out.string());

    std::ifstream in(out);
    std::vector<std::pair<double, double>> positions;
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string name;
        double x = 0.0, y = 0.0;
        fields >> name >> x >> y;
        if (name == "c0") {
            positions.emplace_back(x, y);
        }
    }
    BOOST_REQUIRE_EQUAL(positions.size(), 1U);
    // The Bookshelf seed is degenerate (every cell at the origin); after the
    // flow a movable cell must have been given a real coordinate.
    const bool movedFromSeed = positions.front().first != 0.0 || positions.front().second != 0.0;
    BOOST_TEST(movedFromSeed);
    // c0 is seeded exactly where padA is, and the flow treats fixed cells as
    // blockages, so c0 has to end up clear of padA (which spans x in [0,1]).
    // Requiring a non-zero y instead would be arbitrary: sliding off a pad to
    // the right legitimately leaves y at 0.
    BOOST_TEST(positions.front().first >= 1.0);
}

// Error paths deliberately end the process through ktlog::fatal, so they are
// verified as death tests rather than by catching an exception.

BOOST_AUTO_TEST_CASE(flow_exits_on_an_unknown_algorithm) {
    const ScratchDir dir("badalgo");
    writeBookshelfDesign(dir);
    expectFatalExit(
        [&] {
            FlowMgr flow;
            flow.run("tiny", dir.str(), dir.file("x.pl").string(), "no_such_algo");
        },
        "Unknown placement algorithm");
}

BOOST_AUTO_TEST_CASE(flow_exits_when_the_input_directory_is_missing) {
    const ScratchDir dir("badinput");
    expectFatalExit(
        [&] {
            FlowMgr flow;
            flow.run("absent", dir.str() + "/no_such_dir", dir.file("x.pl").string());
        },
        "cannot scan input directory");
}

BOOST_AUTO_TEST_CASE(flow_exits_when_the_design_files_are_missing) {
    const ScratchDir dir("nofiles");
    // The directory exists but holds no .nodes, which must not be mistaken for
    // an empty design.
    expectFatalExit(
        [&] {
            FlowMgr flow;
            flow.run("absent", dir.str(), dir.file("x.pl").string());
        },
        "Cannot open nodes file");
}
