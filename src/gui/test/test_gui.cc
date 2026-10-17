#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"
#include "gui/kt_layout.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_runner.h"
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
    BOOST_REQUIRE(runner.start({dir.design(), work.string(), ""}));
    BOOST_TEST(!runner.start({dir.design(), work.string(), ""}));
    BOOST_REQUIRE(waitUntil([&recorded] {
        return recorded.result.has_value();
    }));

    BOOST_TEST((recorded.result->outcome == RunResult::Outcome::Placed));
    BOOST_TEST(recorded.result->message == (work / "placed.pl").string());
    BOOST_TEST(fs::exists(work / "placed.pl"));
    BOOST_TEST(fs::exists(work / "ktplace.log"));
    BOOST_TEST(!fs::exists(work / "plots"));

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
    std::unique_ptr<Runner> runner;
    Runner::Callbacks callbacks = recordInto(recorded);
    callbacks.frame = [&recorded, &runner](LayoutFrame frame) {
        recorded.frames.push_back(std::move(frame));
        runner->stop();
    };
    runner = std::make_unique<Runner>(&context, std::move(callbacks));
    BOOST_REQUIRE(runner->start({dir.design(), work.string(), ""}));
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
    BOOST_TEST(macro.red() == 0x6e);
    const QPointF cellCentre = view.worldToScreen({2.5, 1.0});
    const QColor cell = image.pixelColor(cellCentre.toPoint());
    BOOST_TEST(cell.blue() > cell.red() + 0x40);
    BOOST_TEST((image.pixelColor(5, 5) == QColor(0x1e, 0x1e, 0x1e)));
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
