// @file kt_mainWindow.h
// The KTPlace main window, laid out the way EDA tools are: the design view on
// top, and a tabbed dock underneath with the Log and the Tcl Console.
//
// Every menu action is a Tcl command run through the console, so it is echoed
// there and in the log like a typed one; the window holds no engine logic of
// its own. Actions that need the engine are disabled while a script runs.
//
// The central area is the layout view. After every script the window asks the
// engine for a fresh snapshot of the placement, so whatever a command changed
// -- typed, or from a menu -- is on screen when the prompt comes back. Picking a
// cell looks it up on the engine thread and shows it in the Properties panel,
// with its nets drawn over the view.
//
// While a global placement runs, the view follows it live: each iteration's
// placement replaces the picture (without moving the view), the Metrics panel
// plots its wirelength and overflow, and Stop (the toolbar, or Ctrl+.) ends the
// stage with the best placement it has and abandons the rest of the script.

#pragma once

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
    /// Ends the running stage; enabled only while the engine is busy.
    [[nodiscard]] QAction *stopAction() const {
        return stop_;
    }

    /// Ask the engine for the current placement and show it. Runs after every
    /// script; public so tests can wait on it.
    void refreshLayout();
    /// Look up @p cell (or clear the panel) and draw its nets.
    void inspect(std::optional<std::size_t> cell);
    /// The actions that run something on the engine, disabled while it is busy.
    [[nodiscard]] const QList<QAction *> &engineActions() const {
        return engineActions_;
    }

    /// Quote @p word for Tcl, so a path with blanks or brackets stays one word.
    [[nodiscard]] static QString tclQuote(const QString &word);

private:
    void buildMenus();
    void buildToolBar();
    /// One progress report from the running stage.
    void showProgress(const ProgressSample &sample,
                      const std::shared_ptr<const LayoutSnapshot> &layout);
    QAction *engineAction(QMenu *menu, const QString &text, const QString &script,
                          const QKeySequence &shortcut = {});
    void setBusy(bool busy);

    Engine &engine_;
    LayoutView *layout_ = nullptr;
    PropertyPanel *properties_ = nullptr;
    QDockWidget *propertiesDock_ = nullptr;
    QLabel *cursor_ = nullptr;
    LogView *log_ = nullptr;
    TclConsole *console_ = nullptr;
    QDockWidget *logDock_ = nullptr;
    QDockWidget *consoleDock_ = nullptr;
    MetricsView *metrics_ = nullptr;
    QDockWidget *metricsDock_ = nullptr;
    QLabel *status_ = nullptr;
    QList<QAction *> engineActions_;
    QAction *stop_ = nullptr;
    QAction *runFlow_ = nullptr;
    QAction *globalPlace_ = nullptr;
    QAction *legalize_ = nullptr;
    QAction *detailPlace_ = nullptr;
    QAction *zoomFit_ = nullptr;
    std::size_t liveRun_ = 0;  // the run the Metrics panel is plotting
};

}  // namespace ktplace
