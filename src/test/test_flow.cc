// @file test_flow.cc// End-to-end tests for the load -> place -> write flow// These drive FlowMgr exactly the way main() does, on tiny synthetic designs,// so the whole pipeline is covered without any benchmark data on disk.


#define BOOST_TEST_MODULE ktplace_flow
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
// The ktlog::fatal paths throw FatalError, so they are checked in-process.
#include "kt_flowMgr.h"
#include "kt_option.h"

#include "util/kt_log.h"

#include <boost/test/unit_test_suite.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
        std::filesystem::create_directories(file(name).parent_path());
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
/// The design lives in its own directory, because that directory's name is what
/// the reader takes the design name from.
std::filesystem::path designDir(const ScratchDir &dir) {
    return dir.file("tiny");
}

/// The options a run under test gets: a design to read and a work directory to
/// write into. The output and plot paths are derived from the latter, so tests
/// read them back off the options rather than naming them twice.
kt_option optionsFor(const ScratchDir &dir, const std::string &inputPath) {
    kt_option opt;
    opt.inputPath = inputPath;
    opt.workDir = dir.str();
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

/// Require that `fn` raises a FatalError whose message contains `needle`, and
/// that the same text reached stderr as a fatal record.
void expectFatal(const std::function<void()> &fn, const std::string &needle) {
    std::ostringstream captured;
    std::streambuf *original = std::cerr.rdbuf(captured.rdbuf());
    std::string message;
    bool raised = false;
    try {
        fn();
    } catch (const FatalError &e) {
        raised = true;
        message = e.message();
    } catch (...) {
        std::cerr.rdbuf(original);
        throw;
    }
    std::cerr.rdbuf(original);

    BOOST_CHECK_MESSAGE(raised, "expected a FatalError containing \"" << needle << "\"");
    BOOST_CHECK_MESSAGE(message.find(needle) != std::string::npos,
                        "FatalError did not contain \"" << needle << "\"; got: " << message);
    const std::string console = captured.str();
    BOOST_CHECK_MESSAGE(
        console.find("[fatal]") != std::string::npos && console.find(needle) != std::string::npos,
        "stderr did not carry the fatal record; got: " << console);
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
    const kt_option opt = optionsFor(dir, designDir(dir).string());

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
        if (line.find("padA\t") == 0 || line.find("padB\t") == 0) {
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
    const kt_option opt = optionsFor(dir, designDir(dir).string());
    const fs::path plots = opt.getPlotDir();

    FlowMgr flow;
    flow.run(opt);

    // SimPL plots the LSS/HPWL bound it converges against, not the raw
    // per-iteration HPWL series the quadratic placer emitted.
    BOOST_REQUIRE(fs::exists(plots / "simpl_bounds.csv"));
    BOOST_REQUIRE(fs::exists(plots / "simpl_bounds.svg"));
    BOOST_TEST(countNonEmptyLines(plots / "simpl_bounds.csv") > 0);

    std::size_t stills = 0;
    // SimPL's default stills are SVG (simpl_*.svg) under <plotDir>/simpl, nested
    // so they do not collide with the bounds curve. The raster
    // frame_*.ppm stills only appear when a CG trace cadence is requested.
    for (const auto &entry : fs::directory_iterator(plots / "simpl")) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("simpl_", 0) == 0 && entry.path().extension() == ".svg") {
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
    const kt_option opt = optionsFor(dir, dir.str());

    FlowMgr flow;
    // Format auto-detection picks LEF/DEF because the directory holds a .def.
    flow.run(opt);

    BOOST_REQUIRE(fs::exists(opt.getOutputPath()));
    BOOST_TEST(countNonEmptyLines(opt.getOutputPath()) == 4);  // 3 components + 1 pad
}

BOOST_AUTO_TEST_CASE(flow_fails_on_an_unknown_algorithm) {
    const ScratchDir dir("badalgo");
    writeBookshelfDesign(dir);
    expectFatal(
        [&] {
            FlowMgr flow;
            kt_option opt = optionsFor(dir, designDir(dir).string());
            opt.algorithm = "no_such_algo";
            flow.run(opt);
        },
        "Unknown placement algorithm");
}

BOOST_AUTO_TEST_CASE(flow_fails_when_the_input_directory_is_missing) {
    const ScratchDir dir("badinput");
    expectFatal(
        [&] {
            FlowMgr flow;
            flow.run(optionsFor(dir, dir.str() + "/no_such_dir"));
        },
        "cannot read input directory");
}

BOOST_AUTO_TEST_CASE(flow_fails_when_the_design_files_are_missing) {
    const ScratchDir dir("nofiles");
    // The directory exists but holds no .nodes, which must not be mistaken for
    // an empty design.
    expectFatal(
        [&] {
            FlowMgr flow;
            flow.run(optionsFor(dir, dir.str()));
        },
        "no input format recognises");
}
