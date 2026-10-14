// @file test_gui.cc
// Tests for the GUI shell: the engine thread, the log panel, the Tcl console
// and the main window, run headless on Qt's offscreen platform.

#define BOOST_TEST_MODULE ktplace_gui
#define BOOST_TEST_DYN_LINK
#include "gui/kt_engine.h"
#include "gui/kt_logView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_tclConsole.h"
#include "test/kt_testDesign.h"
#include "util/kt_log.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QThread>
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
