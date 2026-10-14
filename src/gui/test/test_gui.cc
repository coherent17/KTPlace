// @file test_gui.cc
// Tests for the GUI shell: the engine thread, the log panel, the Tcl console
// and the main window, run headless on Qt's offscreen platform.

#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "gui/kt_benchmarkPanel.h"
#include "gui/kt_engine.h"
#include "gui/kt_layoutSnapshot.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_metricsView.h"
#include "gui/kt_propertyPanel.h"
#include "gui/kt_runHistory.h"
#include "gui/kt_runsPanel.h"
#include "gui/kt_tclConsole.h"
#include "session/kt_session.h"
#include "test/kt_testDesign.h"
#include "util/kt_log.h"
#include "util/kt_progress.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QThread>
#include <QTreeWidget>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace ktplace;
using ktplace::testing::designDir;
using ktplace::testing::ScratchDir;
using ktplace::testing::writeBookshelfDesign;

namespace {

/// One QApplication for the whole module, on the offscreen platform.
struct QtFixture {
    QtFixture() {
        ::setenv("QT_QPA_PLATFORM", "offscreen", 1);
        app = new QApplication(argc, argv);
    }
    ~QtFixture() {
        delete app;
    }
    int argc = 1;
    char name[16] = "test_gui";
    char *argv[2] = {name, nullptr};
    QApplication *app = nullptr;
};

/// Pump the event loop until @p done holds, or fail after @p ms.
bool waitFor(const std::function<bool()> &done, int ms = 30000) {
    QElapsedTimer timer;
    timer.start();
    while (!done()) {
        if (timer.elapsed() > ms) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return true;
}

/// Keeps stderr quiet for a scope; the log records still reach the sinks.
class QuietStderr {
public:
    QuietStderr() : original(std::cerr.rdbuf(sink.rdbuf())) {}
    ~QuietStderr() {
        std::cerr.rdbuf(original);
    }
    QuietStderr(const QuietStderr &) = delete;
    QuietStderr &operator=(const QuietStderr &) = delete;

private:
    std::ostringstream sink;
    std::streambuf *original;
};

void type(ConsoleInput *input, const QString &text) {
    input->setText(text);
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(input, &enter);
}

}  // namespace

BOOST_GLOBAL_FIXTURE(QtFixture);

BOOST_AUTO_TEST_CASE(the_engine_runs_scripts_off_the_gui_thread) {
    Engine engine;
    BOOST_TEST(engine.commandNames().contains("read_design"));

    QObject receiver;
    std::optional<TclShell::Result> result;
    engine.submit("expr {6 * 7}", &receiver, [&](const TclShell::Result &r) {
        BOOST_TEST((QThread::currentThread() == receiver.thread()));
        result = r;
    });
    BOOST_REQUIRE(waitFor([&] {
        return result.has_value();
    }));
    BOOST_TEST(result->ok);
    BOOST_TEST(result->text == "42");
    BOOST_TEST(!engine.busy());

    // A failure is a result like any other.
    result.reset();
    QuietStderr quiet;
    engine.submit("legalize", &receiver, [&](const TclShell::Result &r) {
        result = r;
    });
    BOOST_REQUIRE(waitFor([&] {
        return result.has_value();
    }));
    BOOST_TEST(!result->ok);
    BOOST_TEST(result->text.find("no design loaded") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(a_result_for_a_destroyed_receiver_is_dropped) {
    Engine engine;
    bool called = false;
    {
        QObject receiver;
        engine.submit("after 50", &receiver, [&](const TclShell::Result &) {
            called = true;
        });
    }
    BOOST_REQUIRE(waitFor([&] {
        return !engine.busy();
    }));
    QCoreApplication::processEvents();
    BOOST_TEST(!called);
}

BOOST_AUTO_TEST_CASE(scripts_run_in_submission_order) {
    Engine engine;
    QObject receiver;
    QStringList seen;
    engine.setGlobal("work_dir", "/w");
    for (const char *script : {"set a 1", "incr a", "incr a", "set ktplace(work_dir)"}) {
        engine.submit(script, &receiver, [&](const TclShell::Result &r) {
            seen << QString::fromStdString(r.text);
        });
    }
    BOOST_REQUIRE(waitFor([&] {
        return seen.size() == 4;
    }));
    BOOST_TEST(seen.join(",").toStdString() == "1,2,3,/w");
}

BOOST_AUTO_TEST_CASE(the_log_view_shows_records_from_any_thread) {
    LogView view;
    QuietStderr quiet;
    ktlog.echo("echo-from-gui-thread");
    QThread *worker = QThread::create([] {
        ktlog.warning("warning-from-worker");
    });
    worker->start();
    worker->wait();
    delete worker;
    ktlog.trace("trace-hidden-by-default");

    BOOST_REQUIRE(waitFor([&] {
        return view.toPlainText().contains("warning-from-worker");
    }));
    BOOST_TEST(view.toPlainText().contains("echo-from-gui-thread"));
    QCoreApplication::processEvents();
    BOOST_TEST(!view.toPlainText().contains("trace-hidden-by-default"));

    view.setShowTrace(true);
    ktlog.trace("trace-now-shown");
    BOOST_REQUIRE(waitFor([&] {
        return view.toPlainText().contains("trace-now-shown");
    }));
}

BOOST_AUTO_TEST_CASE(the_console_runs_typed_commands_and_keeps_history) {
    Engine engine;
    TclConsole console(engine);
    type(console.input(), "set answer 42");
    BOOST_REQUIRE(waitFor([&] {
        return !console.busy();
    }));
    BOOST_TEST(console.transcript().contains("ktplace> set answer 42"));
    BOOST_TEST(console.transcript().endsWith("42"));

    // An open brace continues on the next line; nothing runs until it closes.
    type(console.input(), "proc twice {x} {");
    BOOST_TEST(!console.busy());
    type(console.input(), "  expr {$x * 2} }");
    BOOST_REQUIRE(waitFor([&] {
        return !console.busy();
    }));
    type(console.input(), "twice $answer");
    BOOST_REQUIRE(waitFor([&] {
        return console.transcript().endsWith("84");
    }));

    // Up recalls the last command, and the multi-line one before it whole.
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QApplication::sendEvent(console.input(), &up);
    BOOST_TEST(console.input()->text().toStdString() == "twice $answer");
    QApplication::sendEvent(console.input(), &up);
    BOOST_TEST(console.input()->text().toStdString() == "proc twice {x} {\n  expr {$x * 2} }");
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QApplication::sendEvent(console.input(), &down);
    QApplication::sendEvent(console.input(), &down);
    BOOST_TEST(console.input()->text().isEmpty());
}

BOOST_AUTO_TEST_CASE(the_console_shows_errors_and_completes_command_names) {
    Engine engine;
    TclConsole console(engine);
    QuietStderr quiet;
    type(console.input(), "legalize");
    BOOST_REQUIRE(waitFor([&] {
        return !console.busy();
    }));
    BOOST_TEST(console.transcript().contains("legalize: no design loaded"));

    // Through the real key, which must not move the focus away.
    console.input()->setText("read_d");
    QKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(console.input(), &tab);
    BOOST_TEST(console.input()->text().toStdString() == "read_design ");

    // An empty line completes nothing and lists nothing.
    const QString before = console.transcript();
    console.input()->clear();
    QApplication::sendEvent(console.input(), &tab);
    BOOST_TEST((console.transcript() == before));

    // Ambiguous: extended to the common prefix, candidates listed.
    console.input()->setText("report_p");
    console.input()->onTab();
    BOOST_TEST(console.input()->text().toStdString() == "report_params ");
    console.input()->setText("rep");
    console.input()->onTab();
    BOOST_TEST(console.input()->text().toStdString() == "report_");
    BOOST_TEST(console.transcript().contains("report_hpwl"));
}

BOOST_AUTO_TEST_CASE(menu_actions_run_through_the_console_and_wait_for_the_engine) {
    const ScratchDir dir("gui_window");
    writeBookshelfDesign(dir);
    Engine engine;
    MainWindow window(engine);
    QuietStderr quiet;

    window.console()->run("read_design " +
                          MainWindow::tclQuote(QString::fromStdString(designDir(dir).string())));
    BOOST_REQUIRE(waitFor([&] {
        return !window.console()->busy();
    }));

    QAction *legalize = nullptr;
    for (QAction *a : window.engineActions()) {
        if (a->text() == "&Legalize") {
            legalize = a;
        }
    }
    BOOST_REQUIRE(legalize != nullptr);
    legalize->trigger();
    // Disabled while the engine works, enabled again after.
    BOOST_TEST(window.console()->busy());
    BOOST_TEST(!legalize->isEnabled());
    BOOST_REQUIRE(waitFor([&] {
        return !window.console()->busy();
    }));
    BOOST_TEST(legalize->isEnabled());
    BOOST_TEST(window.console()->transcript().contains("ktplace> legalize"));
    BOOST_REQUIRE(waitFor([&] {
        return window.log()->toPlainText().contains("tcl> legalize");
    }));
}

BOOST_AUTO_TEST_CASE(paths_are_quoted_as_one_tcl_word) {
    BOOST_TEST(MainWindow::tclQuote("/a b/c").toStdString() == "{/a b/c}");
    BOOST_TEST(MainWindow::tclQuote("/odd{dir}").toStdString() == "/odd\\{dir\\}");
    BOOST_TEST(MainWindow::tclQuote("/odd {x} $y").toStdString() == "/odd\\ \\{x\\}\\ \\$y");

    Engine engine;
    QObject receiver;
    std::optional<std::string> text;
    engine.submit("llength [list " + MainWindow::tclQuote("/odd {x} [$y]") + "]", &receiver,
                  [&](const TclShell::Result &r) {
                      text = r.text;
                  });
    BOOST_REQUIRE(waitFor([&] {
        return text.has_value();
    }));
    BOOST_TEST(*text == "1");
}

namespace {

/// The tiny design, read into a session of its own.
struct TinySession {
    ScratchDir dir{"gui_snapshot"};
    ktSession session;
    TinySession() {
        writeBookshelfDesign(dir);
        QuietStderr quiet;
        session.readDesign(designDir(dir).string());
    }
};

/// A snapshot of one 10x10 die with two cells, indexed by hand.
std::shared_ptr<const LayoutSnapshot> twoCells() {
    auto snap = std::make_shared<LayoutSnapshot>();
    snap->die = {0.0, 0.0, 10.0, 10.0};
    snap->cells.push_back({1.0F, 1.0F, 2.0F, 2.0F, LayoutSnapshot::Kind::Movable});
    snap->cells.push_back({6.0F, 6.0F, 3.0F, 3.0F, LayoutSnapshot::Kind::Fixed});
    snap->buildIndex();
    return snap;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_snapshot_copies_the_placement_and_picks_cells) {
    TinySession tiny;
    const auto snap = LayoutSnapshot::capture(tiny.session);
    BOOST_REQUIRE_EQUAL(snap->cells.size(), tiny.session.cells().size());
    BOOST_TEST(snap->die == tiny.session.placementDieBox());
    BOOST_TEST(snap->hpwl == tiny.session.hpwl());
    BOOST_TEST(snap->rows.size() == 1u);

    std::size_t pads = 0;
    for (std::size_t i = 0; i < snap->cells.size(); ++i) {
        const Vertex &v = tiny.session.cells()[i];
        const LayoutSnapshot::Cell &c = snap->cells[i];
        BOOST_TEST(c.x == static_cast<float>(v.x));
        BOOST_TEST(c.w == static_cast<float>(v.width));
        pads += c.kind == LayoutSnapshot::Kind::Terminal ? 1 : 0;
    }
    BOOST_TEST(pads == 2u);
    BOOST_TEST(snap->typicalWidth == 1.0F);
    BOOST_TEST(snap->typicalHeight == 2.0F);

    // Every cell is found at its own centre.
    for (std::size_t i = 0; i < snap->cells.size(); ++i) {
        const LayoutSnapshot::Cell &c = snap->cells[i];
        const auto hit = snap->cellAt(c.x + c.w / 2.0, c.y + c.h / 2.0);
        BOOST_REQUIRE(hit.has_value());
        const LayoutSnapshot::Cell &h = snap->cells[*hit];
        BOOST_TEST((h.x == c.x && h.y == c.y));
    }
}

BOOST_AUTO_TEST_CASE(picking_prefers_the_smallest_cell_and_finds_nothing_in_empty_space) {
    auto snap = std::make_shared<LayoutSnapshot>();
    snap->die = {0.0, 0.0, 100.0, 100.0};
    snap->cells.push_back({0.0F, 0.0F, 50.0F, 50.0F, LayoutSnapshot::Kind::Fixed});
    snap->cells.push_back({10.0F, 10.0F, 1.0F, 2.0F, LayoutSnapshot::Kind::Movable});
    snap->buildIndex();
    BOOST_TEST(snap->cellAt(10.5, 11.0).value() == 1u);  // over the macro, the small one wins
    BOOST_TEST(snap->cellAt(30.0, 30.0).value() == 0u);
    BOOST_TEST(!snap->cellAt(80.0, 80.0).has_value());
}

BOOST_AUTO_TEST_CASE(a_terminal_the_size_of_a_macro_is_drawn_as_one) {
    // The tiny design's pads are cell-sized: they stay pads.
    TinySession tiny;
    for (const LayoutSnapshot::Cell &c : LayoutSnapshot::capture(tiny.session)->cells) {
        BOOST_TEST((c.kind != LayoutSnapshot::Kind::Fixed));
    }
}

BOOST_AUTO_TEST_CASE(the_view_maps_zooms_and_selects) {
    LayoutView view;
    view.resize(400, 400);
    view.setSnapshot(twoCells());

    // Fitted: the die's centre is the window's, and the mapping round-trips.
    const QPointF middle = view.worldToScreen({5.0, 5.0});
    BOOST_TEST(std::abs(middle.x() - 200.0) < 1e-9);
    BOOST_TEST(std::abs(middle.y() - 200.0) < 1e-9);
    const QPointF back = view.screenToWorld(view.worldToScreen({3.25, 7.5}));
    BOOST_TEST(std::abs(back.x() - 3.25) < 1e-9);
    BOOST_TEST(std::abs(back.y() - 7.5) < 1e-9);
    // y grows upward in the design and downward on screen.
    BOOST_TEST(view.worldToScreen({5.0, 9.0}).y() < middle.y());

    // Zooming keeps the point under the cursor where it is.
    const QPointF anchor(100.0, 300.0);
    const QPointF under = view.screenToWorld(anchor);
    const double before = view.scale();
    view.zoomBy(2.0, anchor);
    BOOST_TEST(std::abs(view.scale() - 2.0 * before) < 1e-9);
    const QPointF still = view.worldToScreen(under);
    BOOST_TEST(std::abs(still.x() - anchor.x()) < 1e-6);
    BOOST_TEST(std::abs(still.y() - anchor.y()) < 1e-6);
    view.zoomFit();

    // A click on the first cell selects it; one on empty die clears it.
    std::optional<std::optional<std::size_t>> picked;
    view.onSelect = [&](std::optional<std::size_t> c) {
        picked = c;
    };
    const auto click = [&](QPointF at) {
        QMouseEvent press(QEvent::MouseButtonPress, at, view.mapToGlobal(at), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, at, view.mapToGlobal(at), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&view, &press);
        QApplication::sendEvent(&view, &release);
    };
    click(view.worldToScreen({2.0, 2.0}));
    BOOST_REQUIRE(picked.has_value());
    BOOST_TEST(picked->value() == 0u);
    BOOST_TEST(view.selection().value() == 0u);
    click(view.worldToScreen({4.5, 4.5}));
    BOOST_TEST(!picked->has_value());
    BOOST_TEST(!view.selection().has_value());
}

BOOST_AUTO_TEST_CASE(the_view_draws_cells_and_a_density_map_when_zoomed_out) {
    LayoutView view;
    view.resize(300, 300);
    view.setSnapshot(twoCells());
    const QImage near = view.grab().toImage();
    // The cell's centre is painted in the cell colour, not the die's.
    const QPoint at = view.worldToScreen({2.0, 2.0}).toPoint();
    BOOST_TEST((near.pixelColor(at) != near.pixelColor(view.worldToScreen({4.5, 4.5}).toPoint())));

    // Far out, the 2x2 cell is under a pixel: it lands in the density map and
    // is still drawn, not dropped.
    view.zoomBy(1.0 / 400.0, QPointF(150.0, 150.0));
    BOOST_TEST(2.0 * view.scale() < 1.5);
    const QImage far = view.grab().toImage();
    const QPoint dot = view.worldToScreen({2.0, 2.0}).toPoint();
    const QColor background = far.pixelColor(5, 5);
    bool drawn = false;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            drawn = drawn || far.pixelColor(dot + QPoint(dx, dy)) != background;
        }
    }
    BOOST_TEST(drawn);
}

BOOST_AUTO_TEST_CASE(cell_details_list_the_cell_and_its_nets) {
    TinySession tiny;
    const Graph &g = tiny.session.graph();
    const std::size_t c0 = g.getCellId("c0");
    const auto d = CellDetails::lookup(tiny.session, c0);
    BOOST_REQUIRE(d.has_value());
    BOOST_TEST(d->name.toStdString() == "c0");
    BOOST_TEST(d->kind.toStdString() == "movable");
    BOOST_TEST(d->width == 1.0);
    BOOST_TEST(d->nets.size() == 2u);  // c0 is on n0 and n1
    for (const CellDetails::NetRef &n : d->nets) {
        BOOST_TEST(n.degree == 4u);
        BOOST_TEST(n.others.size() == 3u);
    }
    BOOST_TEST(CellDetails::lookup(tiny.session, g.getCellId("padA"))->kind.toStdString() ==
               "terminal");
    BOOST_TEST(!CellDetails::lookup(tiny.session, 1000).has_value());
    BOOST_TEST(!CellDetails::lookup(ktSession(), 0).has_value());
}

BOOST_AUTO_TEST_CASE(the_window_shows_each_commands_placement_and_inspects_a_pick) {
    const ScratchDir dir("gui_layout");
    writeBookshelfDesign(dir);
    Engine engine;
    MainWindow window(engine);
    QuietStderr quiet;
    BOOST_TEST(!window.layout()->snapshot());

    window.console()->run("read_design " +
                          MainWindow::tclQuote(QString::fromStdString(designDir(dir).string())));
    BOOST_REQUIRE(waitFor([&] {
        return window.layout()->snapshot() != nullptr;
    }));
    const auto first = window.layout()->snapshot();
    BOOST_TEST(first->cells.size() == 6u);

    // After another command, a new snapshot with the moved cells.
    window.console()->run("legalize");
    BOOST_REQUIRE(waitFor([&] {
        return window.layout()->snapshot() != first;
    }));

    window.layout()->setSelection(0);
    window.inspect(0);
    BOOST_REQUIRE(waitFor([&] {
        return window.properties()->tree()->topLevelItemCount() > 3;
    }));
    BOOST_TEST(window.properties()->tree()->topLevelItem(0)->text(1).toStdString() == "c0");
    window.inspect(std::nullopt);
    BOOST_TEST(window.properties()->tree()->topLevelItemCount() == 1);
}

// ------------------------------------------------------------ live placement

BOOST_AUTO_TEST_CASE(a_snapshot_can_show_a_stage_placement_before_it_is_written_back) {
    TinySession tiny;
    const std::size_t n = tiny.session.cells().size();
    std::vector<float> x(n, 2.0F);
    std::vector<float> y(n, 0.0F);
    x[0] = 7.0F;
    const auto snap = LayoutSnapshot::capture(tiny.session, x, y, 123.0);
    BOOST_TEST(snap->hpwl == 123.0);
    BOOST_TEST(snap->cells[0].x == 7.0F);
    BOOST_TEST(snap->cells[1].x == 2.0F);
    BOOST_TEST(snap->cells[0].w == static_cast<float>(tiny.session.cells()[0].width));
    BOOST_TEST(snap->cellAt(7.5, 0.5).value() == 0U);

    // Arrays of the wrong length are ignored, not read past.
    const auto same = LayoutSnapshot::capture(tiny.session, {1.0F}, {1.0F}, 1.0);
    BOOST_TEST(same->cells[0].x == static_cast<float>(tiny.session.cells()[0].x));
}

BOOST_AUTO_TEST_CASE(the_metrics_view_plots_one_run_at_a_time) {
    MetricsView view;
    view.resize(500, 220);
    BOOST_TEST(view.points().empty());
    BOOST_TEST(!view.grab().isNull());  // the empty state paints

    ProgressSample s;
    s.run = 7;
    s.stage = "simpl";
    s.phase = "init";
    s.total = 10;
    for (std::size_t i = 0; i < 3; ++i) {
        s.iter = i;
        s.hpwl = 1e6 / static_cast<double>(i + 1);
        view.addSample(s);
    }
    s.phase = "global";
    s.hpwl = 4e5;
    s.hpwlLower = 2e5;
    s.overflow = 0.0;  // a value, not "unmeasured"
    view.addSample(s);
    BOOST_TEST(view.points().size() == 4U);
    BOOST_TEST(view.title().toStdString() == "simpl run 7");
    BOOST_TEST(!view.grab().isNull());

    s.run = 8;
    view.addSample(s);
    BOOST_TEST(view.points().size() == 1U);
    view.clear();
    BOOST_TEST(view.points().empty());
}

BOOST_AUTO_TEST_CASE(the_engine_streams_a_placement_live_and_can_stop_it) {
    const ScratchDir dir("gui_live");
    writeBookshelfDesign(dir);
    Engine engine;
    QObject receiver;
    QuietStderr quiet;

    std::vector<Engine::LiveUpdate> updates;
    engine.watchProgress(&receiver, [&](const Engine::LiveUpdate &u) {
        BOOST_TEST((QThread::currentThread() == receiver.thread()));
        updates.push_back(u);
    });
    std::optional<TclShell::Result> result;
    const auto run = [&](const std::string &script) {
        result.reset();
        engine.submit(QString::fromStdString(script), &receiver, [&](const TclShell::Result &r) {
            result = r;
        });
        BOOST_REQUIRE(waitFor([&] {
            return result.has_value();
        }));
    };
    run("read_design {" + designDir(dir).string() + "}");
    BOOST_REQUIRE(result->ok);
    run("global_place");
    BOOST_REQUIRE(result->ok);
    QCoreApplication::processEvents();
    BOOST_REQUIRE(!updates.empty());
    bool sawLayout = false;
    for (const Engine::LiveUpdate &u : updates) {
        BOOST_TEST(u.sample.x.empty());  // the positions travel as the snapshot
        if (u.layout) {
            sawLayout = true;
            BOOST_TEST(u.layout->cells.size() == 6U);
            BOOST_TEST(u.layout->hpwl == u.sample.hpwl);
        }
    }
    BOOST_TEST(sawLayout);

    // Stop from the first report of the next run: the stage ends early, and
    // the command after it fails.
    updates.clear();
    bool asked = false;
    engine.watchProgress(&receiver, [&](const Engine::LiveUpdate &u) {
        updates.push_back(u);
        if (!asked) {
            asked = true;
            engine.requestStop();
        }
    });
    run("global_place; after 200; report_hpwl");
    BOOST_TEST(!result->ok);
    BOOST_TEST(result->text == "report_hpwl: stopped on request");
    BOOST_TEST(asked);

    // The next script is not stopped.
    run("report_hpwl");
    BOOST_TEST(result->ok);
    // Asking while idle stops nothing later.
    engine.requestStop();
    run("report_hpwl");
    BOOST_TEST(result->ok);
}

BOOST_AUTO_TEST_CASE(the_window_follows_a_run_and_offers_stop_while_busy) {
    const ScratchDir dir("gui_window_live");
    writeBookshelfDesign(dir);
    Engine engine;
    MainWindow window(engine);
    QuietStderr quiet;
    BOOST_TEST(!window.stopAction()->isEnabled());

    window.console()->run("read_design " +
                          MainWindow::tclQuote(QString::fromStdString(designDir(dir).string())));
    BOOST_REQUIRE(waitFor([&] {
        return window.layout()->snapshot() != nullptr && !window.console()->busy();
    }));
    window.console()->run("global_place; after 300");
    BOOST_TEST(window.stopAction()->isEnabled());
    BOOST_REQUIRE(waitFor([&] {
        return !window.console()->busy();
    }));
    BOOST_TEST(!window.stopAction()->isEnabled());
    BOOST_TEST(!window.metrics()->points().empty());
    BOOST_TEST(window.metrics()->title().startsWith("simpl run"));
}

// ------------------------------------------------------------ run history

namespace {

std::vector<RunStage> flowStages(const QString &algorithm, double finalHpwl, bool legal = true) {
    return {{"read_design", "bookshelf", 900.0, 0.5, false},
            {"global_place", algorithm, 300.0, 2.0, false},
            {"legalize", "abacus", 320.0, 0.25, false},
            {"check_placement", legal ? "legal" : "2 defects", 320.0, 0.0, false},
            {"detail_place", "fastdp", finalHpwl, 0.25, false}};
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_run_record_summarises_its_stages) {
    RunRecord r;
    r.stages = {{"read_design", "bookshelf", 900.0, 0.5, false}};
    BOOST_TEST(r.algorithm().isEmpty());
    BOOST_TEST(!r.globalHpwl().has_value());
    BOOST_TEST(!r.finalHpwl().has_value());  // nothing beyond the read
    BOOST_TEST(r.verdict().isEmpty());
    BOOST_TEST(!r.complete());

    r.stages = flowStages("ntuplace1", 310.0);
    r.stages[1].stopped = true;
    BOOST_TEST(r.algorithm().toStdString() == "ntuplace1");
    BOOST_TEST(*r.globalHpwl() == 300.0);
    BOOST_TEST(*r.finalHpwl() == 310.0);
    BOOST_TEST(r.verdict().toStdString() == "legal");
    BOOST_TEST(r.complete());
    BOOST_TEST(r.stopped());
    BOOST_TEST(r.seconds() == 3.0);
}

BOOST_AUTO_TEST_CASE(the_history_updates_ranks_and_survives_a_restart) {
    const ScratchDir dir("gui_history");
    const QString file = QString::fromStdString(dir.file("runs.json").string());
    const QString designA = QString::fromStdString(dir.file("a").string());
    const QString designB = QString::fromStdString(dir.file("b").string());
    {
        RunHistory h(file);
        BOOST_TEST(h.load());  // no file yet: an empty history
        BOOST_TEST(h.runs().empty());
        BOOST_TEST(h.update(1, designA, flowStages("simpl", 310.0)));
        BOOST_TEST(!h.update(1, designA, flowStages("simpl", 310.0)));  // unchanged
        BOOST_TEST(h.update(1, designA, flowStages("simpl", 305.0)));   // the same run, moved on
        BOOST_TEST(h.runs().size() == 1U);
        BOOST_TEST(h.update(2, designA, flowStages("simpl", 290.0, false)));
        BOOST_TEST(h.update(3, designA, flowStages("ntuplace1", 400.0)));
        // Incomplete runs do not rank.
        BOOST_TEST(h.update(4, designA, {{"read_design", "bookshelf", 1.0, 0.0, false}}));
        BOOST_TEST(h.update(5, designB, flowStages("simpl", 50.0)));
        BOOST_TEST(h.runs().size() == 5U);

        const auto best = h.best(designA, "simpl");
        BOOST_REQUIRE(best.has_value());
        BOOST_TEST(best->hpwl == 290.0);
        BOOST_TEST(best->verdict.toStdString() == "2 defects");
        // The same directory spelt differently is the same design.
        BOOST_TEST(h.best(designA + "/../a", "ntuplace1")->hpwl == 400.0);
        BOOST_TEST(!h.best(designB, "ntuplace1").has_value());

        BOOST_TEST(h.rename(h.runs()[0].id, "first"));
        BOOST_TEST(h.remove(h.runs()[3].id));
        BOOST_TEST(!h.remove("no-such-run"));
        BOOST_TEST(h.save());

        const QStringList tsv = h.toTsv().split('\n', Qt::SkipEmptyParts);
        BOOST_TEST(tsv.size() == 5);  // the header and four runs
        BOOST_TEST(tsv[1].contains("\tfirst\tsimpl\t300\t305\tlegal\t"));
    }
    RunHistory again(file);
    BOOST_REQUIRE(again.load());
    BOOST_REQUIRE(again.runs().size() == 4U);
    BOOST_TEST(again.runs()[0].label.toStdString() == "first");
    BOOST_TEST(again.runs()[0].stages.size() == 5U);
    BOOST_TEST(again.runs()[0].started.isValid());
    // A new session's reads are new runs, not updates of the old ones.
    BOOST_TEST(again.update(1, designA, flowStages("simpl", 1.0)));
    BOOST_TEST(again.runs().size() == 5U);
    BOOST_TEST(again.best(designA, "simpl")->hpwl == 1.0);

    // A file that is not a history is reported, not trusted.
    std::ofstream(dir.file("bad.json")) << "not json";
    RunHistory bad(QString::fromStdString(dir.file("bad.json").string()));
    BOOST_TEST(!bad.load());
}

BOOST_AUTO_TEST_CASE(the_runs_panel_lists_newest_first_and_edits_the_history) {
    RunHistory h;
    h.update(1, "/designs/one", flowStages("simpl", 310.0));
    h.update(2, "/designs/two", flowStages("ntuplace1", 410.0, false));
    RunsPanel panel(h);
    BOOST_REQUIRE(panel.tree()->topLevelItemCount() == 2);
    const QTreeWidgetItem *newest = panel.tree()->topLevelItem(0);
    BOOST_TEST(newest->text(1).toStdString() == "two");
    BOOST_TEST(newest->text(2).toStdString() == "ntuplace1");
    BOOST_TEST(newest->text(5).toStdString() == "2 defects");
    BOOST_TEST(newest->childCount() == 5);

    bool changed = false;
    panel.onChanged = [&] {
        changed = true;
    };
    BOOST_TEST(panel.renameRun(h.runs()[0].id, "baseline"));
    BOOST_TEST(changed);
    BOOST_TEST(panel.tree()->topLevelItem(1)->text(7).toStdString() == "baseline");
    BOOST_TEST(panel.deleteRun(h.runs()[1].id));
    BOOST_TEST(panel.tree()->topLevelItemCount() == 1);
}

BOOST_AUTO_TEST_CASE(the_benchmark_panel_finds_designs_and_shows_their_best_results) {
    const ScratchDir root("gui_bench");
    // Two designs, one nested a level deeper, and a directory that is not one.
    const ScratchDir *dirs[] = {&root};
    static_cast<void>(dirs);
    std::filesystem::create_directories(root.file("suiteA/tiny"));
    std::filesystem::create_directories(root.file("suiteB/deeper/tiny2"));
    std::filesystem::create_directories(root.file("empty"));
    std::ofstream(root.file("suiteA/tiny/tiny.nodes")) << "UCLA nodes 1.0\n";
    std::ofstream(root.file("suiteB/deeper/tiny2/tiny2.def")) << "VERSION 5.8 ;\n";
    std::ofstream(root.file("baseline.tsv")) << "# a comment\n"
                                             << "design\talgorithm\tstatus\thpwl\tverdict\n"
                                             << "suiteA/tiny\tsimpl\tdone\t123.5\tPASS\n"
                                             << "suiteA/tiny\tntuplace1\tfailed\t1\tFAIL\n";

    const QString rootPath = QString::fromStdString(root.str());
    BOOST_TEST(BenchmarkPanel::scan(rootPath).join(",").toStdString() ==
               "suiteA/tiny,suiteB/deeper/tiny2");
    BOOST_TEST(BenchmarkPanel::scan(rootPath + "/no_such_dir").isEmpty());

    RunHistory h;
    const QString tiny = QString::fromStdString(root.file("suiteA/tiny").string());
    h.update(1, tiny, flowStages("simpl", 310.0));
    h.update(2, tiny, flowStages("ntuplace1", 290.0));
    BenchmarkPanel panel(h);
    BOOST_TEST(panel.designs().isEmpty());
    panel.setRoot(rootPath);
    BOOST_REQUIRE(panel.tree()->topLevelItemCount() == 2);
    const QTreeWidgetItem *row = panel.tree()->findItems("suiteA/tiny", Qt::MatchExactly)[0];
    BOOST_TEST(row->text(1).toStdString() == QString::number(310.0, 'e', 4).toStdString());
    BOOST_TEST(row->text(2).toStdString() == QString::number(123.5, 'e', 4).toStdString());
    BOOST_TEST(row->text(3).toStdString() == QString::number(290.0, 'e', 4).toStdString());
    BOOST_TEST(row->text(4).isEmpty());  // a failed baseline row is no baseline
    BOOST_TEST(row->font(3).bold());     // the better of the two
    BOOST_TEST(!row->font(1).bold());

    QString opened;
    panel.onOpen = [&](const QString &path) {
        opened = path;
    };
    Q_EMIT panel.tree()->itemDoubleClicked(const_cast<QTreeWidgetItem *>(row), 0);
    BOOST_TEST(opened == tiny);
}

BOOST_AUTO_TEST_CASE(the_window_records_each_run_in_the_history) {
    const ScratchDir dir("gui_window_history");
    writeBookshelfDesign(dir);
    const QString file = QString::fromStdString(dir.file("runs.json").string());
    const QString design = QString::fromStdString(designDir(dir).string());
    QuietStderr quiet;
    {
        Engine engine;
        MainWindow window(engine);
        window.setHistoryFile(file);
        window.setBenchmarkRoot(QString::fromStdString(dir.str()));
        BOOST_TEST(window.benchmarks()->designs().join(",").toStdString() == "tiny");

        window.runFlowOn(design, "simpl");
        BOOST_REQUIRE(waitFor([&] {
            return !window.history().runs().empty() && window.history().runs().back().complete();
        }));
        const RunRecord &r = window.history().runs().back();
        BOOST_TEST(r.design == design);
        BOOST_TEST(r.algorithm().toStdString() == "simpl");
        BOOST_TEST(r.verdict().toStdString() == "legal");
        BOOST_TEST(window.runs()->tree()->topLevelItemCount() == 1);
        BOOST_TEST(!window.benchmarks()->tree()->topLevelItem(0)->text(1).isEmpty());

        // A command on the same design moves the same run on.
        window.console()->run("global_place -algo ntuplace1");
        BOOST_REQUIRE(waitFor([&] {
            return window.history().runs().back().algorithm() == "ntuplace1";
        }));
        BOOST_TEST(window.history().runs().size() == 1U);
    }
    // Saved as it went, so a new window starts with it.
    Engine engine;
    MainWindow window(engine);
    window.setHistoryFile(file);
    BOOST_TEST(window.history().runs().size() == 1U);
    BOOST_TEST(window.runs()->tree()->topLevelItemCount() == 1);
}
