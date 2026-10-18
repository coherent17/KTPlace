#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "gui/kt_congestion.h"
#include "gui/kt_layout.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_runner.h"
#include "gui/kt_tclConsole.h"
#include "util/kt_log.h"

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QThread>
#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

using namespace ktplace;
using namespace ktplace::gui;
namespace fs = std::filesystem;

namespace {

struct QtApplication {
    QtApplication() : app(argc, argv) {
        ktlog.throwOnFatal(true);
    }
    int argc = 1;
    char name[8] = "ktplace";
    char *argv[2] = {name, nullptr};
    QApplication app;
};

class ScratchDir {
public:
    explicit ScratchDir(const std::string &tag) {
        static int counter = 0;
        path = fs::temp_directory_path() / ("ktplace_gui_" + tag + "_" + std::to_string(++counter));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] fs::path file(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        fs::create_directories(file(name).parent_path());
        std::ofstream out(file(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }

    [[nodiscard]] std::string design() const {
        std::string nodes = "UCLA nodes 1.0\n\nNumNodes : 7\nNumTerminals : 3\n\n";
        for (int i = 0; i < 4; ++i) {
            nodes += "\tc" + std::to_string(i) + "\t1.0\t2.0\n";
        }
        nodes += "\tpadA\t1.0\t2.0\tterminal\n\tpadB\t1.0\t2.0\tterminal\n";
        nodes += "\tblock\t8.0\t6.0\tterminal\n";
        write("tiny/tiny.nodes", nodes);
        write("tiny/tiny.nets",
              "UCLA nets 1.0\n\nNumNets : 2\nNumPins : 8\n\n"
              "NetDegree : 4\tn0\n\tpadA I\t: 0.0\t0.0\n\tc0 O\t: 0.0\t0.0\n"
              "\tc1 O\t: 0.0\t0.0\n\tc2 O\t: 0.0\t0.0\n"
              "NetDegree : 4\tn1\n\tc3 I\t: 0.0\t0.0\n\tpadB O\t: 0.0\t0.0\n"
              "\tc0 I\t: 0.0\t0.0\n\tc1 O\t: 0.0\t0.0\n");
        write("tiny/tiny.pl",
              "UCLA pl 1.0\n\nc0\t0\t0 : N\nc1\t2\t0 : N\nc2\t4\t0 : N\nc3\t6\t0 : N\n"
              "padA\t0\t0 : N /FIXED\npadB\t19\t0 : N /FIXED\nblock\t12\t2 : N /FIXED\n");
        write("tiny/tiny.scl",
              "UCLA scl 1.0\n\nNumRows : 1\n\nCoreRow Horizontal\n  Coordinate   :  0\n"
              "  Height       :  2\n  Sitewidth    :  1\n  Sitespacing  :  1\n"
              "  SubrowOrigin :  0 NumSites :  20\nEnd\n");
        return file("tiny").string();
    }

private:
    fs::path path;
};

bool waitUntil(const std::function<bool()> &done, int milliseconds = 60000) {
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > milliseconds) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents();
    return true;
}

struct Recorded {
    std::vector<LayoutFrame> frames;
    std::optional<RunResult> result;
};

Runner::Callbacks recordInto(Recorded &recorded) {
    return {.frame =
                [&recorded](LayoutFrame frame) {
                    recorded.frames.push_back(std::move(frame));
                },
            .finished =
                [&recorded](const RunResult &result) {
                    recorded.result = result;
                }};
}

std::unique_ptr<ktDM> readDesign(const std::string &dir) {
    std::unique_ptr<InputReader> reader = makeInputReader(dir);
    BOOST_REQUIRE(reader);
    return reader->read(dir);
}

}  // namespace

BOOST_TEST_GLOBAL_FIXTURE(QtApplication);

BOOST_AUTO_TEST_CASE(geometry_tells_movable_cells_pads_and_macros_apart) {
    const ScratchDir dir("geometry");
    const std::unique_ptr<ktDM> db = readDesign(dir.design());
    const auto geometry = LayoutGeometry::of(*db);

    BOOST_REQUIRE_EQUAL(geometry->kind.size(), 7U);
    const std::vector<CellKind> expected{CellKind::Movable, CellKind::Movable, CellKind::Movable,
                                         CellKind::Movable, CellKind::Pad,     CellKind::Pad,
                                         CellKind::Macro};
    BOOST_TEST((geometry->kind == expected));
    BOOST_TEST(geometry->width[6] == 8.0F);
    BOOST_TEST(geometry->typicalWidth == 1.0F);
    BOOST_REQUIRE_EQUAL(geometry->rows.size(), 1U);
    BOOST_TEST(geometry->rows[0].xhi == 20.0F);

    const LayoutFrame frame = LayoutFrame::committed(geometry, *db, "load");
    BOOST_TEST(frame.drawable());
    BOOST_TEST(frame.x[6] == 12.0F);
    BOOST_TEST(frame.y[6] == 2.0F);
}

BOOST_AUTO_TEST_CASE(a_run_streams_frames_and_commits_and_writes_the_placement) {
    const ScratchDir dir("run");
    const fs::path work = dir.file("work");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    const std::string design = dir.design();
    BOOST_REQUIRE(runner.start({design, work.string(), ""}));
    BOOST_TEST(!runner.start({design, work.string(), ""}));
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));

    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Placed));
    BOOST_TEST(recorded.result->message == (work / "placed.pl").string());
    BOOST_TEST(fs::exists(work / "placed.pl"));
    BOOST_TEST(fs::exists(work / "ktplace.log"));
    BOOST_TEST(fs::exists(work / "plots" / "placement.png"));
    BOOST_TEST(fs::exists(work / "plots" / "placement.gif"));
    BOOST_TEST(fs::exists(work / "plots" / "routing_demand.gif"));

    std::vector<std::string> committed;
    std::size_t live = 0;
    for (const LayoutFrame &frame : recorded.frames) {
        BOOST_TEST(frame.drawable());
        if (frame.stage == "load" || frame.stage == "global placement" ||
            frame.stage == "legalization" || frame.stage == "detailed placement") {
            committed.push_back(frame.stage);
        } else {
            ++live;
        }
    }
    const std::vector<std::string> expected{"load", "global placement", "legalization",
                                            "detailed placement"};
    BOOST_TEST(committed == expected, boost::test_tools::per_element());
    BOOST_TEST(live > 0U);
    BOOST_TEST(!runner.running());
}

BOOST_AUTO_TEST_CASE(a_bad_design_fails_the_run_and_not_the_process) {
    const ScratchDir dir("bad");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    BOOST_REQUIRE(runner.start({dir.file("missing").string(), dir.file("work").string(), ""}));
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));
    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Failed));
    BOOST_TEST(recorded.result->message.find("cannot read input directory") != std::string::npos);
    BOOST_TEST(recorded.frames.empty());
}

BOOST_AUTO_TEST_CASE(stop_ends_the_run_before_it_writes) {
    const ScratchDir dir("stop");
    const fs::path work = dir.file("work");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    BOOST_REQUIRE(runner.start({dir.design(), work.string(), ""}));
    runner.stop();
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));
    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Stopped));
    BOOST_TEST(!fs::exists(work / "placed.pl"));
}

BOOST_AUTO_TEST_CASE(the_layout_view_draws_cells_inside_the_die) {
    const ScratchDir dir("view");
    const std::unique_ptr<ktDM> db = readDesign(dir.design());
    LayoutView view;
    view.resize(400, 200);
    view.setFrame(LayoutFrame::committed(LayoutGeometry::of(*db), *db, "load"));
    const QImage image = view.grab().toImage();

    const QPointF macroCentre = view.worldToScreen({16.0, 5.0});
    const QColor macro = image.pixelColor(macroCentre.toPoint());
    BOOST_TEST(macro.red() == 0xd9);
    const QPointF cellCentre = view.worldToScreen({2.5, 1.0});
    const QColor cell = image.pixelColor(cellCentre.toPoint());
    BOOST_TEST(cell.blue() > cell.red() + 0x40);
    BOOST_TEST((image.pixelColor(5, 5) == QColor(0x10, 0x14, 0x18)));
}

BOOST_AUTO_TEST_CASE(clicking_a_cell_selects_it_and_its_nets_are_known) {
    const ScratchDir dir("select");
    const std::unique_ptr<ktDM> db = readDesign(dir.design());
    const auto geometry = LayoutGeometry::of(*db);
    BOOST_REQUIRE_EQUAL(geometry->netPinStart.size(), 3U);
    BOOST_TEST(geometry->netPinStart[2] == 8U);
    BOOST_TEST(geometry->cellNetStart[1] - geometry->cellNetStart[0] == 2U);
    BOOST_TEST(geometry->name[6] == "block");

    LayoutView view;
    view.resize(400, 200);
    view.setFrame(LayoutFrame::committed(geometry, *db, "load"));
    view.selectAt(view.worldToScreen({16.0, 5.0}));
    BOOST_REQUIRE(view.selectedCell().has_value());
    BOOST_TEST(*view.selectedCell() == 6U);
    const QImage image = view.grab().toImage();
    const QColor outline = image.pixelColor(view.worldToScreen({12.0, 5.0}).toPoint());
    BOOST_TEST(outline.red() > 0xe0);

    view.selectAt({2.0, 2.0});
    BOOST_TEST(!view.selectedCell().has_value());
}

BOOST_AUTO_TEST_CASE(a_net_spreads_one_unit_of_demand_evenly_over_its_bounding_box) {
    LayoutGeometry geometry;
    geometry.die = {0.0, 0.0, 4.0, 2.0};
    geometry.width = {0.0F, 0.0F};
    geometry.height = {0.0F, 0.0F};
    geometry.kind = {CellKind::Movable, CellKind::Movable};
    geometry.netPinStart = {0, 2};
    geometry.pinCell = {0, 1};
    geometry.pinDx = {0.0F, 0.0F};
    geometry.pinDy = {0.0F, 0.0F};
    const DemandMap map = estimateDemand(geometry, {0.5F, 3.5F}, {0.5F, 1.5F}, 2);
    BOOST_REQUIRE_EQUAL(map.nx, 2U);
    BOOST_REQUIRE_EQUAL(map.ny, 1U);
    BOOST_TEST(map.at(0, 0) == 0.5F, boost::test_tools::tolerance(1e-6F));
    BOOST_TEST(map.at(1, 0) == 0.5F, boost::test_tools::tolerance(1e-6F));
    BOOST_TEST(map.nets == 1U);
}

BOOST_AUTO_TEST_CASE(every_net_adds_exactly_one_to_the_demand_map) {
    const ScratchDir dir("demand");
    const std::unique_ptr<ktDM> db = readDesign(dir.design());
    const auto geometry = LayoutGeometry::of(*db);
    const LayoutFrame frame = LayoutFrame::committed(geometry, *db, "load");
    const DemandMap map = estimateDemand(*geometry, frame.x, frame.y, 16);
    double total = 0.0;
    for (const float value : map.demand) {
        total += value;
    }
    BOOST_TEST(map.nets == 2U);
    BOOST_TEST(total == 2.0, boost::test_tools::tolerance(1e-4));

    LayoutView view;
    view.resize(400, 200);
    view.setFrame(frame);
    view.showCongestion(true, 16);
    BOOST_TEST(view.showingCongestion());
    BOOST_TEST(!view.grab().toImage().isNull());
}

BOOST_AUTO_TEST_CASE(the_window_runs_a_design_and_logs_it) {
    const ScratchDir dir("window");
    const fs::path work = dir.file("work");
    MainWindow window({dir.design(), work.string(), ""});
    std::optional<RunResult> result;
    window.onFinished([&result](const RunResult &r) {
        result = r;
    });
    window.run();
    BOOST_TEST(window.running());
    BOOST_REQUIRE(waitUntil([&result] {
        return result.has_value();
    }));
    BOOST_TEST((result->outcome == RunResult::Outcome::Placed));
    BOOST_TEST(window.status().startsWith(QStringLiteral("Placed: ")));
    BOOST_TEST(window.layoutView()->frame().stage == "detailed placement");
    BOOST_TEST(window.logView()->toPlainText().contains(
        QStringLiteral("Placement completed successfully!")));
}

BOOST_AUTO_TEST_CASE(the_tcl_console_drives_a_session_step_by_step) {
    const ScratchDir dir("console");
    const std::string design = dir.design();
    const fs::path work = dir.file("work");
    MainWindow window({"", work.string(), ""});
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    TclConsole &console = *window.tclConsole();
    const auto run = [&](const QString &command) {
        const std::size_t before = results.size();
        console.submit(command);
        BOOST_REQUIRE(waitUntil([&] {
            return results.size() > before;
        }));
        return results.back();
    };

    BOOST_TEST(
        (run(QStringLiteral("read_design {%1}").arg(QString::fromStdString(design))).outcome ==
         RunResult::Outcome::Evaluated));
    BOOST_TEST((run(QStringLiteral("place_design")).outcome == RunResult::Outcome::Evaluated));
    const RunResult hpwl = run(QStringLiteral("report_hpwl"));
    BOOST_TEST(hpwl.fromConsole);
    BOOST_TEST(std::stod(hpwl.message) > 0.0);
    BOOST_TEST((run(QStringLiteral("no_such_command")).outcome == RunResult::Outcome::Failed));
    BOOST_TEST((run(QStringLiteral("write_placement")).outcome == RunResult::Outcome::Evaluated));
    BOOST_TEST(fs::exists(work / "placed.pl"));
    BOOST_TEST(window.layoutView()->frame().stage == "detailed placement");
    BOOST_TEST(console.transcript().contains(QStringLiteral("% report_hpwl")));
    BOOST_TEST(console.transcript().contains(QStringLiteral("no_such_command")));
}
