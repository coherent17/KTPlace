// @file kt_mainWindow.h
// The KTPlace main window, laid out the way EDA tools are: the design view on
// top, and a tabbed dock underneath with the Log and the Tcl Console.
//
// Every menu action is a Tcl command run through the console, so it is echoed
// there and in the log like a typed one; the window holds no engine logic of
// its own. Actions that need the engine are disabled while a script runs.
//
// The central area is a placeholder until the layout canvas (phase 3 of
// docs/gui.md) replaces it.

#pragma once

#include <QKeySequence>
#include <QList>
#include <QMainWindow>
#include <QString>

class QAction;
class QDockWidget;
class QLabel;
class QMenu;

namespace ktplace {

class Engine;
class LogView;
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
    LogView *log_ = nullptr;
    TclConsole *console_ = nullptr;
    QDockWidget *logDock_ = nullptr;
    QDockWidget *consoleDock_ = nullptr;
    QLabel *status_ = nullptr;
    QList<QAction *> engineActions_;
};

}  // namespace ktplace
