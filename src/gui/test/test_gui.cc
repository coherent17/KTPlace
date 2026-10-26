#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "gui/kt_congestion.h"
#include "gui/kt_detailsPanel.h"
#include "gui/kt_displayStyle.h"
#include "gui/kt_layout.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_locateDialog.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_namePattern.h"
#include "gui/kt_runner.h"
#include "gui/kt_selectionSet.h"
#include "gui/kt_tclConsole.h"
#include "gui/kt_visibilityPanel.h"
#include "util/kt_history.h"
#include "util/kt_log.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QThread>
#include <QToolBar>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ktplace;
using namespace ktplace::gui;
namespace fs = std::filesystem;

struct IsolatedHome {
    IsolatedHome() {
        const fs::path home =
            fs::temp_directory_path() / ("ktplace_gui_home_" + std::to_string(getpid()));
        fs::create_directories(home);
        setenv("HOME", home.c_str(), 1);
        unsetenv("XDG_CONFIG_HOME");
    }
    ~IsolatedHome() {
        std::error_code ec;
        fs::remove_all(std::getenv("HOME"), ec);
    }
};
BOOST_TEST_GLOBAL_FIXTURE(IsolatedHome);

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

    [[nodiscard]] fs::path getFile(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        fs::create_directories(getFile(name).parent_path());
        std::ofstream out(getFile(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }

    [[nodiscard]] std::string blockDesign() const {
        write("blocks/blocks.nodes",
              "UCLA nodes 1.0\n\nNumNodes : 7\nNumTerminals : 2\n\n"
              "\tc0\t1.0\t2.0\n\tc1\t1.0\t2.0\n\tc2\t1.0\t2.0\n\tc3\t1.0\t2.0\n"
              "\tbig\t10.0\t6.0\n"
              "\tpadA\t1.0\t2.0\tterminal\n\tpadB\t1.0\t2.0\tterminal\n");
        write("blocks/blocks.nets",
              "UCLA nets 1.0\n\nNumNets : 2\nNumPins : 6\n\n"
              "NetDegree : 3\tn0\n\tpadA I\t: 0.0\t0.0\n\tc0 O\t: 0.0\t0.0\n"
              "\tbig O\t: 0.0\t0.0\n"
              "NetDegree : 3\tn1\n\tbig I\t: 0.0\t0.0\n\tpadB O\t: 0.0\t0.0\n"
              "\tc1 O\t: 0.0\t0.0\n");
        write("blocks/blocks.pl",
              "UCLA pl 1.0\n\nc0\t0\t0 : N\nc1\t2\t0 : N\nc2\t4\t0 : N\nc3\t6\t0 : N\n"
              "big\t8\t0 : N\npadA\t0\t0 : N /FIXED\npadB\t19\t0 : N /FIXED\n");
        write("blocks/blocks.scl",
              "UCLA scl 1.0\n\nNumRows : 4\n\n"
              "CoreRow Horizontal\n  Coordinate   :  0\n  Height       :  2\n  Sitewidth    :  1\n"
              "  Sitespacing  :  1\n  SubrowOrigin :  0 NumSites :  20\nEnd\n"
              "CoreRow Horizontal\n  Coordinate   :  2\n  Height       :  2\n  Sitewidth    :  1\n"
              "  Sitespacing  :  1\n  SubrowOrigin :  0 NumSites :  20\nEnd\n"
              "CoreRow Horizontal\n  Coordinate   :  4\n  Height       :  2\n  Sitewidth    :  1\n"
              "  Sitespacing  :  1\n  SubrowOrigin :  0 NumSites :  20\nEnd\n"
              "CoreRow Horizontal\n  Coordinate   :  6\n  Height       :  2\n  Sitewidth    :  1\n"
              "  Sitespacing  :  1\n  SubrowOrigin :  0 NumSites :  20\nEnd\n");
        write("blocks/blocks.aux",
              "RowBasedPlacement : blocks.nodes blocks.nets blocks.pl blocks.scl\n");
        return getFile("blocks").string();
    }

    [[nodiscard]] std::string getDesign() const {
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
        return getFile("tiny").string();
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

QAction *getActionNamed(const QWidget &window, const QString &text) {
    for (QAction *candidate : window.findChildren<QAction *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    return nullptr;
}

template <class Widget>
Widget *getChildNamed(const QWidget &parent, const QString &text) {
    for (Widget *candidate : parent.findChildren<Widget *>()) {
        if (candidate->text() == text) {
            return candidate;
        }
    }
    return nullptr;
}

void sendClick(LayoutView &view, QPointF at, Qt::KeyboardModifiers modifiers) {
    QMouseEvent press(QEvent::MouseButtonPress, at, view.mapToGlobal(at), Qt::LeftButton,
                      Qt::LeftButton, modifiers);
    QApplication::sendEvent(&view, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, at, view.mapToGlobal(at), Qt::LeftButton,
                        Qt::NoButton, modifiers);
    QApplication::sendEvent(&view, &release);
}

struct ShownDesign {
    explicit ShownDesign(const std::string &tag)
        : dir(tag),
          db(readDesign(dir.getDesign())),
          window({"", dir.getFile("work").string(), ""}) {
        window.resize(1000, 700);
        window.getLayoutView()->setFrame(
            LayoutFrame::getCommitted(LayoutGeometry::of(*db), *db, "load"));
    }
    ScratchDir dir;
    std::unique_ptr<ktDM> db;
    MainWindow window;
};

}  // namespace

BOOST_TEST_GLOBAL_FIXTURE(QtApplication);

BOOST_AUTO_TEST_CASE(geometry_tells_movable_cells_pads_and_macros_apart) {
    const ScratchDir dir("geometry");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
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

    const LayoutFrame frame = LayoutFrame::getCommitted(geometry, *db, "load");
    BOOST_TEST(frame.isDrawable());
    BOOST_TEST(frame.x[6] == 12.0F);
    BOOST_TEST(frame.y[6] == 2.0F);
}

BOOST_AUTO_TEST_CASE(a_run_streams_frames_and_commits_and_writes_the_placement) {
    const ScratchDir dir("run");
    const fs::path work = dir.getFile("work");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    const std::string design = dir.getDesign();
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
        BOOST_TEST(frame.isDrawable());
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
    BOOST_TEST(!runner.isRunning());
}

BOOST_AUTO_TEST_CASE(a_bad_design_fails_the_run_and_not_the_process) {
    const ScratchDir dir("bad");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    BOOST_REQUIRE(
        runner.start({dir.getFile("missing").string(), dir.getFile("work").string(), ""}));
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));
    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Failed));
    BOOST_TEST(recorded.result->message.find("cannot read input directory") != std::string::npos);
    BOOST_TEST(recorded.frames.empty());
}

BOOST_AUTO_TEST_CASE(stop_ends_the_run_before_it_writes) {
    const ScratchDir dir("stop");
    const fs::path work = dir.getFile("work");
    Recorded recorded;
    QObject context;
    Runner runner(&context, recordInto(recorded));
    BOOST_REQUIRE(runner.start({dir.getDesign(), work.string(), ""}));
    runner.stop();
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));
    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Stopped));
    BOOST_TEST(!fs::exists(work / "placed.pl"));
}

BOOST_AUTO_TEST_CASE(the_layout_view_draws_cells_inside_the_die) {
    const ScratchDir dir("view");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    LayoutView view;
    view.resize(400, 200);
    view.setFrame(LayoutFrame::getCommitted(LayoutGeometry::of(*db), *db, "load"));
    const QImage image = view.grab().toImage();

    const QPointF macroCentre = view.getWorldToScreen({16.0, 5.0});
    const QColor macro = image.pixelColor(macroCentre.toPoint());
    BOOST_TEST((macro == QColor(0x7d, 0x8a, 0x99)));
    const QPointF cellCentre = view.getWorldToScreen({2.5, 1.0});
    const QColor cell = image.pixelColor(cellCentre.toPoint());
    BOOST_TEST(cell.blue() > cell.red() + 0x40);
    BOOST_TEST((image.pixelColor(5, 5) == QColor(0x10, 0x14, 0x18)));
}

BOOST_AUTO_TEST_CASE(clicking_a_cell_selects_it_and_its_nets_are_known) {
    const ScratchDir dir("select");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    const auto geometry = LayoutGeometry::of(*db);
    BOOST_REQUIRE_EQUAL(geometry->netPinStart.size(), 3U);
    BOOST_TEST(geometry->netPinStart[2] == 8U);
    BOOST_TEST(geometry->cellNetStart[1] - geometry->cellNetStart[0] == 2U);
    BOOST_TEST(geometry->name[6] == "block");

    LayoutView view;
    view.resize(400, 200);
    view.setFrame(LayoutFrame::getCommitted(geometry, *db, "load"));
    view.selectAt(view.getWorldToScreen({16.0, 5.0}));
    BOOST_REQUIRE(view.getSelectedCell().has_value());
    BOOST_TEST(*view.getSelectedCell() == 6U);
    const QImage image = view.grab().toImage();
    const QColor outline = image.pixelColor(view.getWorldToScreen({12.0, 5.0}).toPoint());
    BOOST_TEST(outline.red() > 0xe0);

    view.selectAt({2.0, 2.0});
    BOOST_TEST(!view.getSelectedCell().has_value());
}

BOOST_AUTO_TEST_CASE(a_net_spreads_one_unit_of_demand_evenly_over_its_bounding_box) {
    LayoutGeometry geometry;
    geometry.die = BBox(Point(0.0, 0.0), Point(4.0, 2.0));
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
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    const auto geometry = LayoutGeometry::of(*db);
    const LayoutFrame frame = LayoutFrame::getCommitted(geometry, *db, "load");
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
    BOOST_TEST(view.isShowingCongestion());
    BOOST_TEST(!view.grab().toImage().isNull());
}

BOOST_AUTO_TEST_CASE(the_window_runs_a_design_and_logs_it) {
    const ScratchDir dir("window");
    const fs::path work = dir.getFile("work");
    MainWindow window({dir.getDesign(), work.string(), ""});
    std::optional<RunResult> result;
    window.onFinished([&result](const RunResult &r) {
        result = r;
    });
    window.run();
    BOOST_TEST(window.isRunning());
    BOOST_REQUIRE(waitUntil([&result] {
        return result.has_value();
    }));
    BOOST_TEST((result->outcome == RunResult::Outcome::Placed));
    BOOST_TEST(window.getStatus().startsWith(QStringLiteral("Placed: ")));
    BOOST_TEST(window.getLayoutView()->getFrame().stage == "detailed placement");
    BOOST_TEST(window.getLogView()->toPlainText().contains(
        QStringLiteral("Placement completed successfully!")));
}

BOOST_AUTO_TEST_CASE(the_tcl_console_drives_a_session_step_by_step) {
    const ScratchDir dir("console");
    const std::string design = dir.getDesign();
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    TclConsole &console = *window.getTclConsole();
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
    BOOST_TEST(window.getLayoutView()->getFrame().stage == "detailed placement");
    BOOST_TEST(console.getTranscript().contains(QStringLiteral("% report_hpwl")));
    BOOST_TEST(console.getTranscript().contains(QStringLiteral("no_such_command")));
    const CommandHistory history;
    BOOST_TEST(history.getEntries().back() == "write_placement");
}

BOOST_AUTO_TEST_CASE(run_after_open_places_the_loaded_design_without_reading_it_again) {
    const ScratchDir dir("openrun");
    const std::string design = dir.getDesign();
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    window.openDesign(QString::fromStdString(design));
    BOOST_REQUIRE(waitUntil([&] {
        return !results.empty();
    }));
    BOOST_TEST((results.back().outcome == RunResult::Outcome::Loaded));
    fs::remove_all(design);

    window.run();
    BOOST_REQUIRE(waitUntil([&] {
        return results.size() > 1;
    }));
    BOOST_TEST((results.back().outcome == RunResult::Outcome::Placed));
    BOOST_TEST(fs::exists(work / "placed.pl"));
}

BOOST_AUTO_TEST_CASE(opening_a_design_loads_it_and_read_design_reads_it_again) {
    const ScratchDir dir("opened");
    const std::string design = dir.getDesign();
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    window.openDesign(QString::fromStdString(design));
    BOOST_REQUIRE(waitUntil([&] {
        return !results.empty();
    }));
    BOOST_TEST((results.back().outcome == RunResult::Outcome::Loaded));
    BOOST_TEST(window.getLayoutView()->getFrame().stage == "load");

    window.getTclConsole()->submit(QStringLiteral("read_design"));
    BOOST_REQUIRE(waitUntil([&] {
        return results.size() > 1;
    }));
    BOOST_TEST((results.back().outcome == RunResult::Outcome::Evaluated));
    BOOST_TEST(results.back().message == design);
}

BOOST_AUTO_TEST_CASE(name_patterns_match_like_a_shell_glob) {
    BOOST_TEST(NamePattern("c0").isMatch("c0"));
    BOOST_TEST(!NamePattern("c0").isMatch("c01"));
    BOOST_TEST(NamePattern("c0").isLiteral());
    BOOST_TEST(NamePattern("c*").isMatch("c"));
    BOOST_TEST(NamePattern("c*").isMatch("c123"));
    BOOST_TEST(!NamePattern("c*").isMatch("pad"));
    BOOST_TEST(NamePattern("*clk*").isMatch("u1/clk_buf"));
    BOOST_TEST(NamePattern("a*b*c").isMatch("aXXbYYc"));
    BOOST_TEST(!NamePattern("a*b*c").isMatch("aXXbYY"));
    BOOST_TEST(NamePattern("n?7").isMatch("n17"));
    BOOST_TEST(!NamePattern("n?7").isMatch("n7"));
    BOOST_TEST(NamePattern("*").isMatch(""));
    const std::vector<std::string> names{"c0", "c1", "padA", "c2"};
    BOOST_TEST(matchNames(names, NamePattern("c*")) == (std::vector<std::uint32_t>{0, 1, 3}));
    BOOST_TEST(matchNames(names, NamePattern("c*"), 2).size() == 2U);
}

BOOST_AUTO_TEST_CASE(a_selection_keeps_its_cells_sorted_and_tells_its_listeners) {
    SelectionSet selection;
    int notified = 0;
    selection.subscribe([&notified] {
        ++notified;
    });
    selection.selectCells({5, 1, 5});
    BOOST_TEST(selection.getCells() == (std::vector<std::uint32_t>{1, 5}));
    BOOST_TEST(*selection.focus() == 5U);
    selection.toggleCell(3);
    BOOST_TEST(selection.contains(3));
    BOOST_TEST(*selection.focus() == 3U);
    selection.toggleCell(3);
    BOOST_TEST(!selection.contains(3));
    const std::vector<std::uint32_t> more{9, 1};
    selection.addCells(more);
    BOOST_TEST(selection.getCells() == (std::vector<std::uint32_t>{1, 5, 9}));
    selection.markNets({2, 2, 0});
    BOOST_TEST(selection.getNets() == (std::vector<std::uint32_t>{0, 2}));
    selection.clear();
    BOOST_TEST(selection.empty());
    BOOST_TEST(!selection.focus().has_value());
    BOOST_TEST(notified == 6);
    selection.clear();
    BOOST_TEST(notified == 6);
}

BOOST_AUTO_TEST_CASE(every_display_layer_has_a_key_that_finds_it) {
    const DisplayStyle style;
    for (const DisplayLayer layer : displayLayers) {
        BOOST_TEST((DisplayStyle::fromKey(DisplayStyle::getKey(layer)) == layer));
        BOOST_TEST(!DisplayStyle::getTitle(layer).empty());
    }
    BOOST_TEST(!DisplayStyle::fromKey("bogus").has_value());
    BOOST_TEST(!style.isShown(DisplayLayer::RoutingDemand));
    BOOST_TEST(style.isShown(DisplayLayer::Macros));
    BOOST_TEST((style[DisplayLayer::Macros].color == QColor(0x7d, 0x8a, 0x99)));
    BOOST_TEST((style[DisplayLayer::Pads].color == QColor(0xc9, 0xd1, 0xd9)));
    BOOST_TEST((style[DisplayLayer::MovableMacros].color == QColor(0xd0, 0x8c, 0x45)));
    BOOST_TEST((getLayerOf(CellKind::MovableMacro) == DisplayLayer::MovableMacros));
    BOOST_TEST((getLayerOf(CellKind::Macro) == DisplayLayer::Macros));
    BOOST_TEST((style[DisplayLayer::Selection].color == QColor(0xff, 0x3b, 0x30)));
    BOOST_TEST((style[DisplayLayer::MarkedNets].color == QColor(0xff, 0x3b, 0x30)));
}

BOOST_AUTO_TEST_CASE(geometry_knows_net_names_fences_and_net_extents) {
    const ScratchDir dir("netnames");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    const auto geometry = LayoutGeometry::of(*db);
    BOOST_TEST(geometry->netName == (std::vector<std::string>{"n0", "n1"}),
               boost::test_tools::per_element());
    BOOST_TEST(geometry->fences.empty());
    BOOST_TEST(std::ranges::all_of(geometry->fenceOf, [](std::int32_t fence) {
        return fence < 0;
    }));
    const LayoutFrame frame = LayoutFrame::getCommitted(geometry, *db, "load");
    BOOST_TEST(frame.getNetBounds(0)->getHalfPerimeter() == 4.0);
    BOOST_TEST(frame.getNetBounds(1)->getHalfPerimeter() == 19.0);
    const std::vector<std::uint32_t> both{0, 1};
    BOOST_TEST(frame.getNetsBounds(both)->upperRight.x == 19.5);
    const std::vector<std::uint32_t> cells{0, 6};
    const std::optional<BBox> box = frame.getCellsBounds(cells);
    BOOST_TEST(box->lowerLeft.x == 0.0);
    BOOST_TEST(box->upperRight.x == 20.0);
    BOOST_TEST(box->upperRight.y == 8.0);
}

BOOST_AUTO_TEST_CASE(a_hidden_layer_is_not_drawn) {
    const ScratchDir dir("hidden");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    LayoutView view;
    view.resize(400, 200);
    view.setFrame(LayoutFrame::getCommitted(LayoutGeometry::of(*db), *db, "load"));
    const QPoint macro = view.getWorldToScreen({16.0, 5.0}).toPoint();
    BOOST_TEST((view.grab().toImage().pixelColor(macro) == QColor(0x7d, 0x8a, 0x99)));

    DisplayStyle style = view.getStyle();
    style[DisplayLayer::Macros].visible = false;
    view.setStyle(style);
    BOOST_TEST((view.grab().toImage().pixelColor(macro) == QColor(0x10, 0x14, 0x18)));

    style[DisplayLayer::Macros].visible = true;
    style[DisplayLayer::Macros].color = QColor(0x20, 0xc0, 0x40);
    view.setStyle(style);
    BOOST_TEST(view.grab().toImage().pixelColor(macro).green() == 0xc0);
}

BOOST_AUTO_TEST_CASE(the_panels_follow_the_view) {
    const ScratchDir dir("panels");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    LayoutView &view = *window.getLayoutView();
    view.setFrame(LayoutFrame::getCommitted(LayoutGeometry::of(*db), *db, "load"));
    BOOST_TEST(window.getDetailsPanel()->getTitle() == QStringLiteral("Nothing selected"));

    view.getSelection().selectCells({6});
    BOOST_REQUIRE(waitUntil([&] {
        return window.getDetailsPanel()->getTitle() == QStringLiteral("block");
    }));
    view.getSelection().selectCells({0});
    BOOST_REQUIRE(waitUntil([&] {
        return window.getDetailsPanel()->getTitle() == QStringLiteral("c0");
    }));
    BOOST_TEST(window.getDetailsPanel()->getNetRows() == 2);
    view.getSelection().selectCells({0, 1, 2});
    BOOST_REQUIRE(waitUntil([&] {
        return window.getDetailsPanel()->getTitle() == QStringLiteral("3 cells selected");
    }));
    view.getSelection().clear();
    view.getSelection().markNets({1});
    BOOST_REQUIRE(waitUntil([&] {
        return window.getDetailsPanel()->getTitle() == QStringLiteral("n1");
    }));

    DisplayStyle style = view.getStyle();
    style[DisplayLayer::Rows].visible = false;
    view.setStyle(style);
    BOOST_TEST(!window.getVisibilityPanel()->getStyle().isShown(DisplayLayer::Rows));
    view.showCongestion(true, 16);
    BOOST_TEST(window.getVisibilityPanel()->getStyle().isShown(DisplayLayer::RoutingDemand));
}

BOOST_AUTO_TEST_CASE(locate_finds_cells_and_nets_by_pattern) {
    const ScratchDir dir("locate");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    LocateDialog locate;
    locate.setLayoutGeometry(LayoutGeometry::of(*db));
    locate.search(QStringLiteral("c?"), LocateDialog::Target::Cells);
    BOOST_TEST(locate.isMatch() == (std::vector<std::uint32_t>{0, 1, 2, 3}));
    locate.search(QStringLiteral("pad*"), LocateDialog::Target::Cells);
    BOOST_TEST(locate.isMatch().size() == 2U);
    locate.search(QStringLiteral("*"), LocateDialog::Target::Nets);
    BOOST_TEST(locate.isMatch().size() == 2U);
    locate.search(QStringLiteral("nothing*"), LocateDialog::Target::Nets);
    BOOST_TEST(locate.isMatch().empty());
}

BOOST_AUTO_TEST_CASE(layout_commands_drive_the_view_from_the_console) {
    const ScratchDir dir("layoutcmds");
    const std::string design = dir.getDesign();
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    window.resize(900, 600);
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    const auto evaluate = [&](const QString &command) {
        const std::size_t before = results.size();
        window.getTclConsole()->submit(command);
        BOOST_REQUIRE(waitUntil([&] {
            return results.size() > before;
        }));
        return results.back();
    };
    window.openDesign(QString::fromStdString(design));
    BOOST_REQUIRE(waitUntil([&] {
        return !results.empty();
    }));
    LayoutView &view = *window.getLayoutView();

    RunResult result = evaluate(QStringLiteral("layout_select c*"));
    BOOST_TEST((result.outcome == RunResult::Outcome::Evaluated));
    BOOST_TEST(result.message == "4");
    BOOST_TEST(view.getSelection().getCells().size() == 4U);
    BOOST_TEST(evaluate(QStringLiteral("layout_select -add block")).message == "1");
    BOOST_TEST(view.getSelection().getCells().size() == 5U);
    BOOST_TEST(evaluate(QStringLiteral("layout_mark_nets n*")).message == "2");
    BOOST_TEST(view.getSelection().getNets().size() == 2U);
    BOOST_TEST(evaluate(QStringLiteral("layout_view_selection")).message == "1");

    BOOST_TEST(evaluate(QStringLiteral("layout_layer macros off")).message == "0");
    BOOST_TEST(!view.getStyle().isShown(DisplayLayer::Macros));
    BOOST_TEST(!window.getVisibilityPanel()->getStyle().isShown(DisplayLayer::Macros));
    BOOST_TEST(evaluate(QStringLiteral("layout_layer macros")).message == "0");
    BOOST_TEST(evaluate(QStringLiteral("layout_layer")).message.find("macros off") !=
               std::string::npos);
    result = evaluate(QStringLiteral("layout_layer bogus on"));
    BOOST_TEST((result.outcome != RunResult::Outcome::Evaluated));
    BOOST_TEST(result.message.find("unknown layer") != std::string::npos);

    const fs::path image = dir.getFile("view.png");
    BOOST_TEST(
        evaluate(QStringLiteral("layout_capture %1").arg(QString::fromStdString(image.string())))
            .message == image.string());
    BOOST_TEST(fs::exists(image));
    BOOST_TEST(!QImage(QString::fromStdString(image.string())).isNull());

    evaluate(QStringLiteral("layout_view_area 0 0 10 10"));
    BOOST_TEST(view.getWorldToScreen({5.0, 5.0}).x() == view.width() / 2.0);
    result = evaluate(QStringLiteral("layout_view_area 0 0 ten 10"));
    BOOST_TEST((result.outcome != RunResult::Outcome::Evaluated));

    evaluate(QStringLiteral("layout_clear"));
    BOOST_TEST(view.getSelection().empty());
    BOOST_TEST(evaluate(QStringLiteral("layout_view_selection")).message == "0");
}

BOOST_AUTO_TEST_CASE(the_cursor_is_reported_and_shift_drag_zooms_to_the_box) {
    const ScratchDir dir("cursor");
    const std::unique_ptr<ktDM> db = readDesign(dir.getDesign());
    const fs::path work = dir.getFile("work");
    MainWindow window({"", work.string(), ""});
    LayoutView &view = *window.getLayoutView();
    view.resize(400, 300);
    view.setFrame(LayoutFrame::getCommitted(LayoutGeometry::of(*db), *db, "load"));

    const QPointF at = view.getWorldToScreen({4.0, 1.0});
    QMouseEvent move(QEvent::MouseMove, at, view.mapToGlobal(at), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QApplication::sendEvent(&view, &move);
    BOOST_TEST(window.getCursorText() == QStringLiteral("x 4.0   y 1.0"));

    const QPointF from = view.getWorldToScreen({12.0, 8.0});
    const QPointF to = view.getWorldToScreen({20.0, 2.0});
    QMouseEvent press(QEvent::MouseButtonPress, from, view.mapToGlobal(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::ShiftModifier);
    QApplication::sendEvent(&view, &press);
    QMouseEvent drag(QEvent::MouseMove, to, view.mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
                     Qt::ShiftModifier);
    QApplication::sendEvent(&view, &drag);
    QMouseEvent release(QEvent::MouseButtonRelease, to, view.mapToGlobal(to), Qt::LeftButton,
                        Qt::NoButton, Qt::ShiftModifier);
    QApplication::sendEvent(&view, &release);
    const QPointF centre = view.getScreenToWorld({view.width() / 2.0, view.height() / 2.0});
    BOOST_TEST(centre.x() == 16.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(centre.y() == 5.0, boost::test_tools::tolerance(1e-6));
    BOOST_TEST(view.getSelection().empty());
}

BOOST_AUTO_TEST_CASE(details_sit_on_the_left_and_visibility_starts_hidden_on_the_right) {
    const ScratchDir dir("docks");
    MainWindow window({"", dir.getFile("work").string(), ""});
    auto *details = window.findChild<QDockWidget *>(QStringLiteral("details"));
    auto *visibility = window.findChild<QDockWidget *>(QStringLiteral("visibility"));
    BOOST_REQUIRE(details != nullptr);
    BOOST_REQUIRE(visibility != nullptr);
    BOOST_TEST((window.dockWidgetArea(details) == Qt::LeftDockWidgetArea));
    BOOST_TEST((window.dockWidgetArea(visibility) == Qt::RightDockWidgetArea));
    BOOST_TEST(!details->isHidden());
    BOOST_TEST(visibility->isHidden());

    auto *tools = window.findChild<QToolBar *>(QStringLiteral("view"));
    BOOST_REQUIRE(tools != nullptr);
    BOOST_REQUIRE(tools->actions().contains(visibility->toggleViewAction()));
    BOOST_TEST(visibility->toggleViewAction()->isCheckable());
    visibility->toggleViewAction()->trigger();
    BOOST_TEST(!visibility->isHidden());
    visibility->toggleViewAction()->trigger();
    BOOST_TEST(visibility->isHidden());
}

BOOST_AUTO_TEST_CASE(about_and_keys_open_without_blocking_the_window) {
    const ScratchDir dir("notes");
    MainWindow window({"", dir.getFile("work").string(), ""});
    window.resize(900, 600);
    window.show();
    for (const QString &name :
         {QStringLiteral("About KTPlace"), QStringLiteral("Mouse and Keys")}) {
        QAction *open = getActionNamed(window, name);
        BOOST_REQUIRE(open != nullptr);
        const QList<QMessageBox *> before = window.findChildren<QMessageBox *>();
        open->trigger();
        QPointer<QMessageBox> note;
        for (QMessageBox *box : window.findChildren<QMessageBox *>()) {
            if (!before.contains(box)) {
                note = box;
            }
        }
        BOOST_REQUIRE(note != nullptr);
        BOOST_TEST(!note->isHidden());
        BOOST_TEST(!note->isModal());
        BOOST_TEST(window.frameGeometry().contains(note->geometry().center()));
        note->close();
        BOOST_REQUIRE(waitUntil([&note] {
            return note.isNull();
        }));
    }
}

BOOST_AUTO_TEST_CASE(the_visibility_panel_controls_the_view) {
    ShownDesign shown("visibilitypanel");
    VisibilityPanel &panel = *shown.window.getVisibilityPanel();
    LayoutView &view = *shown.window.getLayoutView();
    auto *macros = getChildNamed<QCheckBox>(panel, QStringLiteral("Fixed macros"));
    BOOST_REQUIRE(macros != nullptr);
    BOOST_TEST(macros->isChecked());
    macros->click();
    BOOST_TEST(!view.getStyle().isShown(DisplayLayer::Macros));
    macros->click();
    BOOST_TEST(view.getStyle().isShown(DisplayLayer::Macros));

    getChildNamed<QPushButton>(panel, QStringLiteral("Hide All"))->click();
    BOOST_TEST(std::ranges::none_of(displayLayers, [&view](DisplayLayer layer) {
        return view.getStyle().isShown(layer);
    }));
    BOOST_TEST(!macros->isChecked());
    getChildNamed<QPushButton>(panel, QStringLiteral("Show All"))->click();
    BOOST_TEST(std::ranges::all_of(displayLayers, [&view](DisplayLayer layer) {
        return view.getStyle().isShown(layer);
    }));
    BOOST_TEST(view.isShowingCongestion());
    getChildNamed<QPushButton>(panel, QStringLiteral("Defaults"))->click();
    BOOST_TEST((view.getStyle() == DisplayStyle()));
    BOOST_TEST(!view.isShowingCongestion());

    view.showCongestion(true, 16);
    BOOST_TEST(getChildNamed<QCheckBox>(panel, QStringLiteral("Routing demand"))->isChecked());
}

BOOST_AUTO_TEST_CASE(clicking_a_net_in_details_marks_it_and_double_click_zooms_to_it) {
    ShownDesign shown("detailsnets");
    LayoutView &view = *shown.window.getLayoutView();
    DetailsPanel &details = *shown.window.getDetailsPanel();
    view.getSelection().selectCells({3});
    BOOST_REQUIRE(waitUntil([&] {
        return details.getTitle() == QStringLiteral("c3");
    }));
    const QList<QTableWidget *> tables = details.findChildren<QTableWidget *>();
    BOOST_REQUIRE(tables.size() == 2);
    QTableWidget *nets = tables[1];
    BOOST_REQUIRE(nets->rowCount() == 1);
    BOOST_TEST(nets->item(0, 0)->text() == QStringLiteral("n1"));
    BOOST_TEST(nets->item(0, 1)->text() == QStringLiteral("4"));
    BOOST_TEST(nets->item(0, 2)->text() == QStringLiteral("19.0"));

    Q_EMIT nets->itemClicked(nets->item(0, 0));
    BOOST_TEST(view.getSelection().getNets() == (std::vector<std::uint32_t>{1}));
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{3}));

    view.zoomFit();
    Q_EMIT nets->itemDoubleClicked(nets->item(0, 0));
    const QPointF centre = view.getScreenToWorld({view.width() / 2.0, view.height() / 2.0});
    BOOST_TEST(centre.x() == 10.0, boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(locate_buttons_select_and_zoom) {
    ShownDesign shown("locatebuttons");
    LayoutView &view = *shown.window.getLayoutView();
    QAction *open = getActionNamed(shown.window, QStringLiteral("Locate..."));
    BOOST_REQUIRE(open != nullptr);
    open->trigger();
    LocateDialog &locate = *shown.window.getLocateDialog();
    BOOST_TEST(locate.isVisible());
    locate.search(QStringLiteral("c*"), LocateDialog::Target::Cells);
    getChildNamed<QPushButton>(locate, QStringLiteral("Select"))->click();
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{0, 1, 2, 3}));

    locate.search(QStringLiteral("n0"), LocateDialog::Target::Nets);
    view.zoomFit();
    getChildNamed<QPushButton>(locate, QStringLiteral("Select and Zoom"))->click();
    BOOST_TEST(view.getSelection().getNets() == (std::vector<std::uint32_t>{0}));
    const QPointF centre = view.getScreenToWorld({view.width() / 2.0, view.height() / 2.0});
    BOOST_TEST(centre.x() == 3.5, boost::test_tools::tolerance(1e-6));
    locate.hide();
}

BOOST_AUTO_TEST_CASE(menu_actions_select_none_and_zoom) {
    ShownDesign shown("menuactions");
    LayoutView &view = *shown.window.getLayoutView();
    view.getSelection().selectCells({6});
    getActionNamed(shown.window, QStringLiteral("Zoom to Selection"))->trigger();
    QPointF centre = view.getScreenToWorld({view.width() / 2.0, view.height() / 2.0});
    BOOST_TEST(centre.x() == 16.0, boost::test_tools::tolerance(1e-6));
    getActionNamed(shown.window, QStringLiteral("Select None"))->trigger();
    BOOST_TEST(view.getSelection().empty());
    getActionNamed(shown.window, QStringLiteral("Zoom Fit"))->trigger();
    centre = view.getScreenToWorld({view.width() / 2.0, view.height() / 2.0});
    const auto &die = view.getFrame().geometry->die;
    BOOST_TEST(centre.x() == (die.lowerLeft.x + die.upperRight.x) / 2.0,
               boost::test_tools::tolerance(1e-6));
}

BOOST_AUTO_TEST_CASE(ctrl_click_adds_and_removes_cells) {
    ShownDesign shown("ctrlclick");
    LayoutView &view = *shown.window.getLayoutView();
    view.resize(400, 300);
    view.zoomFit();
    sendClick(view, view.getWorldToScreen({16.0, 5.0}), Qt::NoModifier);
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{6}));
    sendClick(view, view.getWorldToScreen({6.5, 1.0}), Qt::ControlModifier);
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{3, 6}));
    sendClick(view, view.getWorldToScreen({16.0, 5.0}), Qt::ControlModifier);
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{3}));
    sendClick(view, view.getWorldToScreen({16.0, 5.0}), Qt::NoModifier);
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{6}));
}

BOOST_AUTO_TEST_CASE(opened_designs_are_listed_under_recent_designs) {
    const ScratchDir dir("recent");
    const std::string design = dir.getDesign();
    MainWindow window({"", dir.getFile("work").string(), ""});
    std::optional<RunResult> result;
    window.onFinished([&result](const RunResult &r) {
        result = r;
    });
    window.openDesign(QString::fromStdString(design));
    BOOST_REQUIRE(waitUntil([&result] {
        return result.has_value();
    }));
    const QSettings settings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace"));
    BOOST_TEST(settings.value(QStringLiteral("recentDesigns")).toStringList().front() ==
               QString::fromStdString(design));

    QMenu *recent = nullptr;
    for (QMenu *menu : window.findChildren<QMenu *>()) {
        if (menu->title() == QStringLiteral("Recent Designs")) {
            recent = menu;
        }
    }
    BOOST_REQUIRE(recent != nullptr);
    Q_EMIT recent->aboutToShow();
    BOOST_TEST(recent->actions().front()->text() == QString::fromStdString(design));
    BOOST_TEST(!recent->actions().front()->isEnabled() == window.isRunning());
}

BOOST_AUTO_TEST_CASE(a_layout_saved_by_an_older_version_is_ignored) {
    {
        QSettings settings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace"));
        QMainWindow old;
        auto *dock = new QDockWidget(QStringLiteral("Visibility"), &old);
        dock->setObjectName(QStringLiteral("visibility"));
        old.addDockWidget(Qt::LeftDockWidgetArea, dock);
        settings.setValue(QStringLiteral("window/geometry"), old.saveGeometry());
        settings.setValue(QStringLiteral("window/state"), old.saveState(1));
    }
    const ScratchDir dir("oldlayout");
    MainWindow window({"", dir.getFile("work").string(), ""});
    auto *visibility = window.findChild<QDockWidget *>(QStringLiteral("visibility"));
    BOOST_TEST((window.dockWidgetArea(visibility) == Qt::RightDockWidgetArea));
    BOOST_TEST(visibility->isHidden());
    QSettings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace")).clear();
}

BOOST_AUTO_TEST_CASE(a_click_only_selects_and_connectivity_is_shown_on_request) {
    ShownDesign shown("connectivity");
    LayoutView &view = *shown.window.getLayoutView();
    (void)shown.window.grab();
    view.zoomFit();
    const QPoint between = view.getWorldToScreen({10.0, 1.0}).toPoint();
    const QColor empty = view.grab().toImage().pixelColor(between);

    sendClick(view, view.getWorldToScreen({6.5, 1.0}), Qt::NoModifier);
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{3}));
    BOOST_TEST(view.getSelection().getNets().empty());
    BOOST_TEST((view.grab().toImage().pixelColor(between) == empty));

    getActionNamed(shown.window, QStringLiteral("Show Connectivity"))->trigger();
    BOOST_TEST(view.getSelection().getNets() == (std::vector<std::uint32_t>{1}));
    const QColor line = view.grab().toImage().pixelColor(between);
    BOOST_TEST(line.red() > 0xc0);
    BOOST_TEST(line.red() > line.green() + 0x60);

    getActionNamed(shown.window, QStringLiteral("Hide Connectivity"))->trigger();
    BOOST_TEST(view.getSelection().getNets().empty());
    BOOST_TEST(view.getSelection().getCells() == (std::vector<std::uint32_t>{3}));
    BOOST_TEST((view.grab().toImage().pixelColor(between) == empty));

    view.getSelection().selectCells({0, 3});
    BOOST_TEST(view.showConnectivity() == 2U);
}

BOOST_AUTO_TEST_CASE(layout_connectivity_shows_and_hides_from_tcl) {
    const ScratchDir dir("connectivitycmd");
    const std::string design = dir.getDesign();
    MainWindow window({"", dir.getFile("work").string(), ""});
    std::vector<RunResult> results;
    window.onFinished([&results](const RunResult &r) {
        results.push_back(r);
    });
    const auto evaluate = [&](const QString &command) {
        const std::size_t before = results.size();
        window.getTclConsole()->submit(command);
        BOOST_REQUIRE(waitUntil([&] {
            return results.size() > before;
        }));
        return results.back().message;
    };
    window.openDesign(QString::fromStdString(design));
    BOOST_REQUIRE(waitUntil([&] {
        return !results.empty();
    }));
    evaluate(QStringLiteral("layout_select c0"));
    BOOST_TEST(evaluate(QStringLiteral("layout_connectivity")) == "2");
    BOOST_TEST(window.getLayoutView()->getSelection().getNets().size() == 2U);
    BOOST_TEST(evaluate(QStringLiteral("layout_connectivity off")) == "0");
    BOOST_TEST(window.getLayoutView()->getSelection().getNets().empty());
}

BOOST_AUTO_TEST_CASE(a_large_movable_cell_is_a_movable_macro_with_its_own_layer) {
    const ScratchDir dir("blocks");
    const std::unique_ptr<ktDM> db = readDesign(dir.blockDesign());
    const auto geometry = LayoutGeometry::of(*db);
    BOOST_REQUIRE_EQUAL(geometry->name[4], "big");
    BOOST_TEST((geometry->kind[4] == CellKind::MovableMacro));
    BOOST_TEST((geometry->kind[0] == CellKind::Movable));
    BOOST_TEST((geometry->kind[5] == CellKind::Pad));

    LayoutView view;
    view.resize(400, 300);
    view.setFrame(LayoutFrame::getCommitted(geometry, *db, "load"));
    const QPoint block = view.getWorldToScreen({13.0, 3.0}).toPoint();
    BOOST_TEST((view.grab().toImage().pixelColor(block) == QColor(0xd0, 0x8c, 0x45)));
    const QPoint cell = view.getWorldToScreen({0.5, 1.0}).toPoint();
    BOOST_TEST((view.grab().toImage().pixelColor(cell) != QColor(0xd0, 0x8c, 0x45)));

    DisplayStyle style = view.getStyle();
    style[DisplayLayer::MovableMacros].visible = false;
    view.setStyle(style);
    BOOST_TEST((view.grab().toImage().pixelColor(block) == QColor(0x10, 0x14, 0x18)));
    view.selectAt(view.getWorldToScreen({13.0, 3.0}));
    BOOST_TEST(!view.getSelectedCell().has_value());

    view.setStyle(DisplayStyle());
    view.selectAt(view.getWorldToScreen({13.0, 3.0}));
    BOOST_REQUIRE(view.getSelectedCell().has_value());
    BOOST_TEST(*view.getSelectedCell() == 4U);

    DetailsPanel details;
    details.describe(view.getFrame(), view.getSelection());
    BOOST_TEST(details.getTitle() == QStringLiteral("big"));
    const QTableWidget *properties = details.findChildren<QTableWidget *>().front();
    BOOST_TEST(properties->item(0, 1)->text() == QStringLiteral("Movable macro"));
}

BOOST_AUTO_TEST_CASE(the_clicked_net_stays_highlighted_while_the_table_refreshes) {
    ShownDesign shown("stablenets");
    LayoutView &view = *shown.window.getLayoutView();
    DetailsPanel &details = *shown.window.getDetailsPanel();
    view.getSelection().selectCells({0});
    BOOST_REQUIRE(waitUntil([&] {
        return details.getTitle() == QStringLiteral("c0");
    }));
    QTableWidget *nets = details.findChildren<QTableWidget *>()[1];
    BOOST_REQUIRE(nets->rowCount() == 2);
    nets->sortByColumn(2, Qt::DescendingOrder);
    const auto rowOf = [nets](const QString &name) {
        for (int row = 0; row < nets->rowCount(); ++row) {
            if (nets->item(row, 0)->text() == name) {
                return row;
            }
        }
        return -1;
    };
    const auto highlighted = [nets] {
        QStringList names;
        for (const QModelIndex &index : nets->selectionModel()->selectedRows()) {
            names.push_back(nets->item(index.row(), 0)->text());
        }
        return names;
    };
    BOOST_REQUIRE(rowOf(QStringLiteral("n1")) == 0);

    Q_EMIT nets->itemClicked(nets->item(rowOf(QStringLiteral("n0")), 0));
    BOOST_TEST(view.getSelection().getNets() == (std::vector<std::uint32_t>{0}));
    QCoreApplication::processEvents();
    BOOST_TEST(highlighted() == QStringList{QStringLiteral("n0")});

    LayoutFrame moved = view.getFrame();
    moved.x[2] += 30.0F;
    view.setFrame(moved);
    details.describe(view.getFrame(), view.getSelection());
    BOOST_TEST(nets->rowCount() == 2);
    BOOST_TEST(highlighted() == QStringList{QStringLiteral("n0")});
    BOOST_TEST(nets->item(rowOf(QStringLiteral("n0")), 2)->text() == QStringLiteral("34.0"));
    BOOST_TEST(rowOf(QStringLiteral("n0")) == 0);

    view.getSelection().markNets({1});
    QCoreApplication::processEvents();
    BOOST_TEST(highlighted() == QStringList{QStringLiteral("n1")});
}

BOOST_AUTO_TEST_CASE(visibility_starts_hidden_even_when_it_was_left_open) {
    const ScratchDir dir("visibilityclosed");
    {
        MainWindow window({"", dir.getFile("work").string(), ""});
        window.show();
        auto *visibility = window.findChild<QDockWidget *>(QStringLiteral("visibility"));
        visibility->toggleViewAction()->trigger();
        BOOST_REQUIRE(!visibility->isHidden());
        window.close();
    }
    const QSettings saved(QStringLiteral("KTPlace"), QStringLiteral("KTPlace"));
    BOOST_REQUIRE(saved.contains(QStringLiteral("window/state")));

    MainWindow reopened({"", dir.getFile("work").string(), ""});
    auto *visibility = reopened.findChild<QDockWidget *>(QStringLiteral("visibility"));
    auto *details = reopened.findChild<QDockWidget *>(QStringLiteral("details"));
    BOOST_TEST(visibility->isHidden());
    BOOST_TEST(!details->isHidden());
    QSettings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace")).clear();
}

BOOST_AUTO_TEST_CASE(recent_designs_are_remembered_by_absolute_path) {
    const ScratchDir dir("recentabsolute");
    const fs::path design = dir.getDesign();
    const fs::path relative = fs::relative(design);
    BOOST_REQUIRE(relative.is_relative());
    MainWindow window({"", dir.getFile("work").string(), ""});
    std::optional<RunResult> result;
    window.onFinished([&result](const RunResult &r) {
        result = r;
    });
    window.openDesign(QString::fromStdString(relative.string()));
    BOOST_REQUIRE(waitUntil([&result] {
        return result.has_value();
    }));
    const QSettings settings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace"));
    BOOST_TEST(settings.value(QStringLiteral("recentDesigns")).toStringList().front() ==
               QString::fromStdString(fs::absolute(relative).lexically_normal().string()));
    QSettings(QStringLiteral("KTPlace"), QStringLiteral("KTPlace")).clear();
}
