// @file kt_tclConsole.h
// The Tcl Console panel: a transcript and a prompt, like tclsh in a window.
//
// What is typed goes to the Engine as a script. A line that leaves a command
// open (an unclosed brace or quote) continues on the next one, and the prompt
// changes to say so. Up and Down walk the history; Tab completes a KTPlace
// command name. Menu actions are run through run() so they appear here exactly
// as if typed, which makes the transcript a script of the session.
//
// One script runs at a time: the prompt is disabled until its result is back.

#pragma once

#include <QLineEdit>
#include <QStringList>
#include <QWidget>
#include <functional>

class QLabel;
class QPlainTextEdit;

namespace ktplace {

class Engine;

/// The input line: a QLineEdit that also takes Up, Down and Tab.
class ConsoleInput : public QLineEdit {
public:
    using QLineEdit::QLineEdit;

    std::function<void()> onUp;
    std::function<void()> onDown;
    std::function<void()> onTab;

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
};

class TclConsole : public QWidget {
public:
    explicit TclConsole(Engine &engine, QWidget *parent = nullptr);

    /// Run @p script as if typed: echoed at the prompt, then evaluated.
    void run(const QString &script);

    /// Called with `true` when a script starts and `false` when its result is
    /// back, so the window can disable its actions while the engine is busy.
    std::function<void(bool)> onBusyChanged;

    [[nodiscard]] bool busy() const {
        return busy_;
    }
    [[nodiscard]] QString transcript() const;
    [[nodiscard]] ConsoleInput *input() const {
        return input_;
    }

private:
    void submitLine();
    void historyStep(int delta);
    void complete();
    void setBusy(bool busy);
    void appendText(const QString &text, const QColor &color = QColor());

    Engine &engine_;
    QPlainTextEdit *view_ = nullptr;
    QLabel *prompt_ = nullptr;
    ConsoleInput *input_ = nullptr;
    QString pending_;  // lines of a command not complete yet
    QStringList history_;
    int historyPos_ = 0;
    bool busy_ = false;
};

}  // namespace ktplace
