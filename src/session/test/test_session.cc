// @file test_session.cc
// Tests for ktSession: the stages called one at a time, in more than one order,
// on a tiny synthetic design, with the placement read back between them.

#define BOOST_TEST_MODULE ktplace_session
#define BOOST_TEST_DYN_LINK
#include "datamodel/kt_optionDb.h"
#include "detailPlacer/fastdp/fastdp_dm.h"
#include "legalizer/abacus/abacus_dm.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/simpl_dm.h"
#include "session/kt_session.h"
#include "test/kt_testDesign.h"
#include "util/kt_progress.h"

#include <boost/test/unit_test.hpp>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace ktplace;
using ktplace::testing::countNonEmptyLines;
using ktplace::testing::designDir;
using ktplace::testing::expectFatal;
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

BOOST_AUTO_TEST_CASE(session_rejects_an_out_of_range_detail_window) {
    // KTPLACE_DP_WINDOW used to be read twice: unchecked by the flow, then 2..14
    // by FastDP, and the flow's copy won. Now only FastDP reads it, so 20 is
    // refused with a warning and the stage runs with its own window.
    const ScratchDir dir("session_dpwindow");
    ktSession session;
    loadTiny(session, dir);
    session.legalize();

    ::setenv("KTPLACE_DP_WINDOW", "20", 1);
    std::ostringstream captured;
    std::streambuf *original = std::cerr.rdbuf(captured.rdbuf());
    try {
        session.detailPlace();
    } catch (...) {
        std::cerr.rdbuf(original);
        ::unsetenv("KTPLACE_DP_WINDOW");
        throw;
    }
    std::cerr.rdbuf(original);
    ::unsetenv("KTPLACE_DP_WINDOW");

    BOOST_TEST(captured.str().find("fastdp window 20 is outside 2..14") != std::string::npos);
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
    expectFatal(
        [] {
            ktSession session;
            session.legalize();
        },
        "legalize: no design loaded");
    expectFatal(
        [] {
            ktSession session;
            session.globalPlace("simpl");
        },
        "global_place: no design loaded");
    expectFatal(
        [] {
            ktSession session;
            session.detailPlace();
        },
        "detail_place: no design loaded");
    expectFatal(
        [] {
            const ktSession session;
            session.writeDesign("/dev/null");
        },
        "write_design: no design loaded");
    expectFatal(
        [] {
            const ktSession session;
            (void)session.hpwl();
        },
        "report_hpwl: no design loaded");
    expectFatal(
        [] {
            const ktSession session;
            (void)session.checkPlacement();
        },
        "check_placement: no design loaded");
}

BOOST_AUTO_TEST_CASE(session_fails_on_an_unknown_algorithm) {
    const ScratchDir dir("session_badalgo");
    writeBookshelfDesign(dir);
    expectFatal(
        [&] {
            ktSession session;
            session.readDesign(designDir(dir).string());
            session.globalPlace("no_such_algo");
        },
        "Unknown placement algorithm");
}

BOOST_AUTO_TEST_CASE(session_fails_when_the_input_directory_is_missing) {
    const ScratchDir dir("session_badinput");
    expectFatal(
        [&] {
            ktSession session;
            session.readDesign(dir.str() + "/no_such_dir");
        },
        "cannot read input directory");
}

BOOST_AUTO_TEST_CASE(session_fails_when_the_output_cannot_be_opened) {
    const ScratchDir dir("session_badoutput");
    writeBookshelfDesign(dir);
    expectFatal(
        [&] {
            ktSession session;
            session.readDesign(designDir(dir).string());
            session.writeDesign(dir.str() + "/no_such_dir/placed.pl");
        },
        "Cannot open output file");
}

BOOST_AUTO_TEST_CASE(session_logs_each_stage_and_starts_over_on_a_new_read) {
    const ScratchDir dir("session_stagelog");
    ktSession session;
    BOOST_TEST(session.designPath().empty());
    BOOST_TEST(session.designSerial() == 0U);
    loadTiny(session, dir);
    BOOST_TEST(session.designPath() == designDir(dir).string());
    BOOST_TEST(session.designSerial() == 1U);

    session.globalPlace("simpl");
    session.legalize();
    session.reportPlacementCheck("after legalization");
    session.detailPlace();

    const auto &log = session.stageLog();
    BOOST_REQUIRE(log.size() == 5U);
    BOOST_TEST(log[0].stage == "read_design");
    BOOST_TEST(log[0].detail == "bookshelf");
    BOOST_TEST(log[1].stage == "global_place");
    BOOST_TEST(log[1].detail == "simpl");
    BOOST_TEST(!log[1].stopped);
    BOOST_TEST(log[2].stage == "legalize");
    BOOST_TEST(log[2].detail == "abacus");
    BOOST_TEST(log[3].stage == "check_placement");
    BOOST_TEST(log[3].detail == "legal");
    BOOST_TEST(log[4].stage == "detail_place");
    BOOST_TEST(log[4].hpwl == session.hpwl());
    for (const ktSession::StageRecord &r : log) {
        BOOST_TEST(r.seconds >= 0.0);
    }

    session.readDesign(designDir(dir).string());
    BOOST_TEST(session.designSerial() == 2U);
    BOOST_TEST(session.stageLog().size() == 1U);
}

namespace {

/// Installs a Progress observer for the scope, and removes it again.
struct ObserveProgress {
    std::vector<ProgressSample> samples;
    std::function<void(const ProgressSample &)> also;

    explicit ObserveProgress(bool positions) {
        Progress::instance().clearStop();
        Progress::instance().setObserver(
            [this](ProgressSample &&s) {
                if (also) {
                    also(s);
                }
                samples.push_back(std::move(s));
            },
            positions);
    }
    ~ObserveProgress() {
        Progress::instance().clearObserver();
        Progress::instance().clearStop();
    }
    ObserveProgress(const ObserveProgress &) = delete;
    ObserveProgress &operator=(const ObserveProgress &) = delete;
};

}  // namespace

BOOST_AUTO_TEST_CASE(simpl_reports_each_iteration_to_an_observer) {
    const ScratchDir dir("session_progress");
    ktSession session;
    loadTiny(session, dir);
    ObserveProgress watch(/*positions=*/true);
    session.globalPlace("simpl");

    BOOST_REQUIRE(!watch.samples.empty());
    bool sawGlobal = false;
    for (const ProgressSample &s : watch.samples) {
        BOOST_TEST(s.stage == "simpl");
        BOOST_TEST((s.phase == "init" || s.phase == "global"));
        BOOST_TEST(s.run == watch.samples.front().run);
        BOOST_TEST(s.iter < s.total);
        BOOST_TEST(std::isfinite(s.hpwl));
        BOOST_TEST(s.x.size() == session.cells().size());
        BOOST_TEST(s.y.size() == session.cells().size());
        sawGlobal = sawGlobal || s.phase == "global";
    }
    BOOST_TEST(sawGlobal);

    // A second run is numbered apart from the first.
    const std::size_t firstRun = watch.samples.front().run;
    watch.samples.clear();
    session.globalPlace("simpl");
    BOOST_REQUIRE(!watch.samples.empty());
    BOOST_TEST(watch.samples.front().run > firstRun);
}

BOOST_AUTO_TEST_CASE(simpl_leaves_positions_out_unless_asked) {
    const ScratchDir dir("session_progress_lean");
    ktSession session;
    loadTiny(session, dir);
    ObserveProgress watch(/*positions=*/false);
    session.globalPlace("simpl");
    BOOST_REQUIRE(!watch.samples.empty());
    BOOST_TEST(watch.samples.front().x.empty());
}

BOOST_AUTO_TEST_CASE(simpl_stops_on_request_and_keeps_a_placement) {
    const ScratchDir dir("session_stop");
    ktSession session;
    loadTiny(session, dir);
    ObserveProgress watch(/*positions=*/false);
    // Ask for the stop from inside the first global iteration's report, as a
    // Stop button would between two iterations. The run then ships that
    // iteration's look-ahead-legalized placement, which lies inside the die.
    watch.also = [](const ProgressSample &s) {
        if (s.phase == "global") {
            Progress::instance().requestStop();
        }
    };
    session.globalPlace("simpl");

    BOOST_REQUIRE(!watch.samples.empty());
    BOOST_TEST(watch.samples.back().phase == "global");
    BOOST_TEST(watch.samples.back().iter == 0U);
    BOOST_TEST(session.stageLog().back().stopped);
    BOOST_TEST(std::isfinite(session.hpwl()));
    BOOST_TEST(insideDie(session));
}

BOOST_AUTO_TEST_CASE(simpl_stopped_in_the_warm_up_skips_the_global_phase) {
    const ScratchDir dir("session_stop_init");
    ktSession session;
    loadTiny(session, dir);
    ObserveProgress watch(/*positions=*/false);
    watch.also = [](const ProgressSample &) {
        Progress::instance().requestStop();
    };
    session.globalPlace("simpl");
    BOOST_REQUIRE(watch.samples.size() == 1U);
    BOOST_TEST(watch.samples.front().phase == "init");
    BOOST_TEST(session.stageLog().back().stopped);
    BOOST_TEST(std::isfinite(session.hpwl()));
}

BOOST_AUTO_TEST_CASE(stage_options_apply_when_set_and_leave_the_defaults_otherwise) {
    const ScratchDir dir("session_options");
    ktSession session;
    loadTiny(session, dir);
    const ktDM &db = session.design();
    // RatioPlacer writes its result into the database, so it takes it mutably;
    // only its parameters are read here.
    ktDM &mutableDb = const_cast<ktDM &>(db);
    OptionDb::instance().clear();

    // Unset: each stage keeps its own defaults, not the options' declared ones.
    BOOST_TEST(simpl::buildSimplDM(db, PlotOptions{}).useFences());
    BOOST_TEST(fastdp::buildFastdpDM(db).params().localReorderWindow == 6U);
    BOOST_TEST(abacus::buildAbacusDM(db).params().maxRowDistance == 0U);
    BOOST_TEST(RatioPlacer(mutableDb).params().targetLeafCells == 32U);
    BOOST_TEST(RatioPlacer(mutableDb).params().maxRatioRetries == 24U);

    // Set: they apply.
    OptionDb &opts = OptionDb::instance();
    opts.set("simpl", "fences", false);
    opts.set("abacus", "max_row_dist", 3);
    opts.set("ntuplace1", "leaf_cells", 12);
    opts.set("ntuplace1", "verbose", true);
    BOOST_TEST(!simpl::buildSimplDM(db, PlotOptions{}).useFences());
    BOOST_TEST(abacus::buildAbacusDM(db).params().maxRowDistance == 3U);
    BOOST_TEST(RatioPlacer(mutableDb).params().targetLeafCells == 12U);
    BOOST_TEST(RatioPlacer(mutableDb).params().verbose);
    BOOST_TEST(RatioPlacer(mutableDb).params().maxLevels == 40U);  // still the placer's

    // A legal FastDP window is used; the placement stays legal either way.
    session.legalize();
    opts.set("fastdp", "window", 3);
    session.detailPlace();
    BOOST_TEST(session.checkPlacement().empty());
    OptionDb::instance().clear();
}
