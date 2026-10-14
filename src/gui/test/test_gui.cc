// @file test_gui.cc
// Tests for the GUI shell: the engine thread, the log panel, the Tcl console
// and the main window, run headless on Qt's offscreen platform.

#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "gui/kt_engine.h"
#include "gui/kt_layoutSnapshot.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_propertyPanel.h"
#include "gui/kt_tclConsole.h"
#include "session/kt_session.h"
#include "test/kt_testDesign.h"
#include "util/kt_log.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QThread>
#include <QTreeWidget>
#include <boost/test/unit_test.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

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
