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

#pragma once

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QString>
#include <cstddef>
#include <optional>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;

namespace ktplace {

class Engine;
class LayoutView;
class LogView;
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
    QLabel *status_ = nullptr;
    QList<QAction *> engineActions_;
};

}  // namespace ktplace
