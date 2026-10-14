// @file kt_mainWindow.h
// The KTPlace main window: the layout view in the middle, Properties on the
// right, and a tabbed dock underneath with the Log, the Tcl Console, Metrics
// and Runs. See docs/gui.md for how it drives the engine.
//
// Every menu and toolbar action is a Tcl command run through the console, so it
// is echoed there and in the log like a typed one; actions that need the engine
// are disabled while a script runs. After each script the window shows a fresh
// snapshot of the placement and brings the run history up to date. While a
// stage runs, the view and the Metrics plot follow it, and Stop (Ctrl+.) ends it
// with the best placement it has.

#pragma once

#include "gui/kt_runHistory.h"

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <cstddef>
#include <memory>
#include <optional>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;

namespace ktplace {

struct LayoutSnapshot;
struct ProgressSample;
class Engine;
class LayoutView;
class LogView;
class MetricsView;
class PropertyPanel;
class RunsPanel;
class TclConsole;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(Engine &engine, QWidget *parent = nullptr);

    [[nodiscard]] TclConsole *console() const {
        return console_;
    }
    [[nodiscard]] LogView *log() const {
        return log_;
    }
    [[nodiscard]] LayoutView *layout() const {
        return layout_;
    }
    [[nodiscard]] PropertyPanel *properties() const {
        return properties_;
    }
    [[nodiscard]] MetricsView *metrics() const {
        return metrics_;
    }
    [[nodiscard]] RunsPanel *runs() const {
        return runs_;
    }
    [[nodiscard]] const RunHistory &history() const {
        return history_;
    }

    /// Keep the run history in @p file (loaded now, saved after each change);
    /// until this is called it is kept in memory only.
    void setHistoryFile(const QString &file);
    /// List the designs under @p root in the Runs panel.
    void setBenchmarkRoot(const QString &root);

    /// The whole flow after a read, as one script: what Run Full Flow runs.
    [[nodiscard]] static QString flowScript(const QString &algorithm);
    /// Read @p design and run the flow on it with @p algorithm, in the console.
    void runFlowOn(const QString &design, const QString &algorithm);
    /// Bring the run history up to date with the session's stage log, and show
    /// the current placement. Both run after every script; public for tests.
    void recordRun();
    void refreshLayout();
    /// Look up @p cell (or clear the panel) and draw its nets.
    void inspect(std::optional<std::size_t> cell);

    /// Ends the running stage; enabled only while the engine is busy.
    [[nodiscard]] QAction *stopAction() const {
        return stop_;
    }
    /// The actions that run something on the engine, disabled while it is busy.
    [[nodiscard]] const QList<QAction *> &engineActions() const {
        return engineActions_;
    }

    /// Quote @p word for Tcl, so a path with blanks or brackets stays one word.
    [[nodiscard]] static QString tclQuote(const QString &word);

private:
    QDockWidget *addDock(const QString &title, QWidget *widget, Qt::DockWidgetArea area);
    void buildMenus();
    QAction *engineAction(QMenu *menu, const QString &text, const QString &script,
                          const QKeySequence &shortcut = {});
    /// An engine action whose script needs a path from a file dialog first.
    void pathAction(QMenu *menu, const QString &text, const QString &command,
                    const QKeySequence &shortcut, int dialog);
    void showProgress(const ProgressSample &sample,
                      const std::shared_ptr<const LayoutSnapshot> &layout);
    void setBusy(bool busy);

    Engine &engine_;
    RunHistory history_;
    LayoutView *layout_ = nullptr;
    PropertyPanel *properties_ = nullptr;
    LogView *log_ = nullptr;
    TclConsole *console_ = nullptr;
    MetricsView *metrics_ = nullptr;
    RunsPanel *runs_ = nullptr;
    QList<QDockWidget *> docks_;  // in View-menu order
    QLabel *status_ = nullptr;
    QLabel *cursor_ = nullptr;
    QList<QAction *> engineActions_;
    QAction *stop_ = nullptr;
    std::size_t liveRun_ = 0;  // the run the Metrics panel is plotting
};

}  // namespace ktplace
