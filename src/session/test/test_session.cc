// @file test_session.cc
// Tests for ktSession: the stages called one at a time, in more than one order,
// on a tiny synthetic design, with the placement read back between them.

#define BOOST_TEST_MODULE ktplace_session
#define BOOST_TEST_DYN_LINK
#include "session/kt_session.h"
#include "test/kt_testDesign.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <utility>

using namespace ktplace;
using ktplace::testing::countNonEmptyLines;
using ktplace::testing::designDir;
using ktplace::testing::expectFatalExit;
using ktplace::testing::ScratchDir;
using ktplace::testing::writeBookshelfDesign;
namespace fs = std::filesystem;

namespace {

/// A session with the tiny design loaded, drawing nothing.
void loadTiny(ktSession &session, const ScratchDir &dir) {
    writeBookshelfDesign(dir);
    session.readDesign(designDir(dir).string());
    BOOST_REQUIRE(session.hasDesign());
}

/// Every movable cell lies inside the die.
bool insideDie(const ktSession &session) {
    const std::array<double, 4> box = session.placementDieBox();
    for (const Vertex &v : session.cells()) {
        if (v.isFixed || v.isTerminal) {
            continue;
        }
        if (v.x < box[0] || v.y < box[1] || v.x + v.width > box[2] || v.y + v.height > box[3]) {
            return false;
        }
    }
    return true;
}

}  // namespace

BOOST_AUTO_TEST_CASE(session_starts_empty_and_draws_nothing_by_default) {
    const ktSession session;
    BOOST_TEST(!session.hasDesign());
    BOOST_TEST(session.plotOptions().dir.empty());
    BOOST_TEST(session.multiRowCells() == 0u);
}

BOOST_AUTO_TEST_CASE(session_reads_a_design_and_exposes_it_read_only) {
    const ScratchDir dir("session_read");
    ktSession session;
    loadTiny(session, dir);
    session.reportDesign();

    // One vertex per node, terminals included, and the views agree.
    BOOST_TEST(session.cells().size() == 6u);
    BOOST_TEST(session.graph().getNumCells() == 6u);
    BOOST_TEST(&session.graph() == &session.design().getGraph());
    BOOST_TEST(&session.die() == &session.design().die());

    // The float copy a draw call takes matches the live positions.
    const auto [x, y] = session.positions();
    BOOST_REQUIRE(x.size() == 6u);
    for (std::size_t v = 0; v < x.size(); ++v) {
        BOOST_TEST(x[v] == static_cast<float>(session.cells()[v].x));
        BOOST_TEST(y[v] == static_cast<float>(session.cells()[v].y));
    }
    BOOST_TEST(session.multiRowCells() == 0u);
}

// The whole flow, one call at a time, with the placement queried between the
// stages the way a console would.
BOOST_AUTO_TEST_CASE(session_runs_each_stage_individually) {
    const ScratchDir dir("session_stages");
    ktSession session;
    loadTiny(session, dir);

    const double seeded = session.hpwl();
    BOOST_TEST(std::isfinite(seeded));
    BOOST_TEST(seeded >= 0.0);

    session.globalPlace("simpl");
    const double placed = session.hpwl();
    BOOST_TEST(std::isfinite(placed));
    BOOST_TEST(placed > 0.0);
    BOOST_TEST(insideDie(session));
    // A die 20 sites wide by one row: no two pins can be further apart than its
    // half-perimeter, once per net.
    const std::array<double, 4> box = session.placementDieBox();
    BOOST_TEST(placed <= 2.0 * ((box[2] - box[0]) + (box[3] - box[1])));

    session.legalize();
    BOOST_TEST(session.checkPlacement().empty());
    BOOST_TEST(session.reportPlacementCheck("after legalization").empty());
    const double legal = session.hpwl();

    session.detailPlace();
    BOOST_TEST(session.checkPlacement().empty());
    // FastDP only accepts moves that do not lose wirelength.
    BOOST_TEST(session.hpwl() <= legal + 1e-9);

    // Nothing to draw into, so no animation.
    session.renderFinalImage();
    BOOST_TEST(!session.finishAnimation());
}

BOOST_AUTO_TEST_CASE(session_legalizes_without_a_global_placement_first) {
    // Stages are not chained: legalize starts from whatever the database holds,
    // here the reader's seed.
    const ScratchDir dir("session_order");
    ktSession session;
    loadTiny(session, dir);

    session.legalize();
    BOOST_TEST(session.checkPlacement().empty());
    BOOST_TEST(insideDie(session));

    // And a stage may run again on its own output.
    session.detailPlace();
    session.legalize();
    BOOST_TEST(session.checkPlacement().empty());
}

BOOST_AUTO_TEST_CASE(session_writes_the_current_placement) {
    const ScratchDir dir("session_write");
    ktSession session;
    loadTiny(session, dir);
    session.globalPlace("simpl");
    session.legalize();

    const fs::path out = dir.file("placed.pl");
    session.writeDesign(out.string());
    BOOST_REQUIRE(fs::exists(out));
    BOOST_TEST(countNonEmptyLines(out) == 6u);

    // What was written is what the session holds.
    std::map<std::string, std::pair<double, double>> written;
    std::ifstream in(out);
    for (std::string line; std::getline(in, line);) {
        std::istringstream fields(line);
        std::string name;
        double x = 0.0;
        double y = 0.0;
        fields >> name >> x >> y;
        written[name] = {x, y};
    }
    for (const Vertex &v : session.cells()) {
        BOOST_REQUIRE(written.contains(v.name));
        BOOST_TEST(written[v.name].first == v.x, boost::test_tools::tolerance(1e-6));
        BOOST_TEST(written[v.name].second == v.y, boost::test_tools::tolerance(1e-6));
    }
}

BOOST_AUTO_TEST_CASE(session_reading_again_replaces_the_design) {
    const ScratchDir dir("session_reread");
    ktSession session;
    loadTiny(session, dir);
    session.globalPlace("simpl");
    session.legalize();

    session.readDesign(designDir(dir).string());
    // Back to the reader's seed, and a fresh database rather than the old one.
    ktSession fresh;
    fresh.readDesign(designDir(dir).string());
    BOOST_REQUIRE(session.cells().size() == fresh.cells().size());
    for (std::size_t v = 0; v < fresh.cells().size(); ++v) {
        BOOST_TEST(session.cells()[v].x == fresh.cells()[v].x);
        BOOST_TEST(session.cells()[v].y == fresh.cells()[v].y);
    }
}

BOOST_AUTO_TEST_CASE(session_stages_fail_before_a_design_is_loaded) {
    expectFatalExit(
        [] {
            ktSession session;
            session.legalize();
        },
        "legalize: no design loaded");
    expectFatalExit(
        [] {
            ktSession session;
            session.globalPlace("simpl");
        },
        "global_place: no design loaded");
    expectFatalExit(
        [] {
            ktSession session;
            session.detailPlace();
        },
        "detail_place: no design loaded");
    expectFatalExit(
        [] {
            const ktSession session;
            session.writeDesign("/dev/null");
        },
        "write_design: no design loaded");
    expectFatalExit(
        [] {
            const ktSession session;
            (void)session.hpwl();
        },
        "report_hpwl: no design loaded");
    expectFatalExit(
        [] {
            const ktSession session;
            (void)session.checkPlacement();
        },
        "check_placement: no design loaded");
}

BOOST_AUTO_TEST_CASE(session_fails_on_an_unknown_algorithm) {
    const ScratchDir dir("session_badalgo");
    writeBookshelfDesign(dir);
    expectFatalExit(
        [&] {
            ktSession session;
            session.readDesign(designDir(dir).string());
            session.globalPlace("no_such_algo");
        },
        "Unknown placement algorithm");
}

BOOST_AUTO_TEST_CASE(session_fails_when_the_input_directory_is_missing) {
    const ScratchDir dir("session_badinput");
    expectFatalExit(
        [&] {
            ktSession session;
            session.readDesign(dir.str() + "/no_such_dir");
        },
        "cannot read input directory");
}

BOOST_AUTO_TEST_CASE(session_fails_when_the_output_cannot_be_opened) {
    const ScratchDir dir("session_badoutput");
    writeBookshelfDesign(dir);
    expectFatalExit(
        [&] {
            ktSession session;
            session.readDesign(designDir(dir).string());
            session.writeDesign(dir.str() + "/no_such_dir/placed.pl");
        },
        "Cannot open output file");
}
