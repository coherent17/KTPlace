// @file test_flow.cc// End-to-end tests for the load -> place -> write flow// These drive FlowMgr exactly the way main() does, on tiny synthetic designs,// so the whole pipeline is covered without any benchmark data on disk.


#define BOOST_TEST_MODULE ktplace_flow
#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>
// The ktlog::fatal paths throw FatalError, so they are checked in-process.
#include "kt_flowMgr.h"
#include "kt_option.h"

#include "datamodel/kt_optionDb.h"
#include "test/kt_testDesign.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace ktplace;
using ktplace::testing::countNonEmptyLines;
using ktplace::testing::designDir;
using ktplace::testing::expectFatal;
using ktplace::testing::ScratchDir;
using ktplace::testing::writeBookshelfDesign;
namespace fs = std::filesystem;

namespace {

/// The options a run under test gets: a design to read and a work directory to
/// write into. The output and plot paths are derived from the latter, so tests
/// read them back off the options rather than naming them twice.
///
/// Drawing is off: it is most of a run's time and none of these tests look at
/// it. The one that does turns it back on (see below).
kt_option optionsFor(const ScratchDir &dir, const std::string &inputPath) {
    kt_option opt;
    opt.inputPath = inputPath;
    opt.workDir = dir.str();
    opt.plot = false;
    return opt;
}

/// Small pictures for the test that draws, set the way a run is configured
/// (add_option visualizer ...), and cleared again when it ends. The frame size
/// and count do not change what is exercised: every writer still runs.
struct SmallPictures {
    SmallPictures() {
        auto &db = OptionDb::instance();
        db.set("visualizer", "anim", true);
        db.set("visualizer", "anim_max_frames", 40);
        db.set("visualizer", "anim_zoom", 1.0);
        db.set("visualizer", "final_zoom", 1.0);
    }
    ~SmallPictures() {
        OptionDb::instance().clear();
    }
    SmallPictures(const SmallPictures &) = delete;
    SmallPictures &operator=(const SmallPictures &) = delete;
};

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
    const SmallPictures pictures;
    kt_option opt = optionsFor(dir, designDir(dir).string());
    opt.plot = true;
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
            OptionDb::instance().set("flow", "algorithm", OptionValue{std::string("no_such_algo")});
            const kt_option opt = optionsFor(dir, designDir(dir).string());
            flow.run(opt);
        },
        "unknown placer");
    OptionDb::instance().clear();  // the option outlives the run in-process
}

BOOST_AUTO_TEST_CASE(flow_reads_the_placer_from_its_tcl_script) {
    const ScratchDir dir("tclalgo");
    writeBookshelfDesign(dir);
    const fs::path script = fs::path(dir.str()) / "run.tcl";
    std::ofstream(script) << "add_option flow algorithm no_such_algo\n";
    expectFatal(
        [&] {
            FlowMgr flow;
            kt_option opt = optionsFor(dir, designDir(dir).string());
            opt.tclFile = script.string();
            flow.run(opt);
        },
        "unknown placer 'no_such_algo'");
    OptionDb::instance().clear();  // the option outlives the run in-process
}

BOOST_AUTO_TEST_CASE(flow_fails_on_a_broken_tcl_script) {
    const ScratchDir dir("badtcl");
    writeBookshelfDesign(dir);
    const fs::path script = fs::path(dir.str()) / "run.tcl";
    std::ofstream(script) << "add_option nonesuch grid 64\n";
    expectFatal(
        [&] {
            FlowMgr flow;
            kt_option opt = optionsFor(dir, designDir(dir).string());
            opt.tclFile = script.string();
            flow.run(opt);
        },
        "is not a known component");
    OptionDb::instance().clear();  // the option outlives the run in-process
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
