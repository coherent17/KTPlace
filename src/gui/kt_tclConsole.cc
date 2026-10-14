// @file kt_tclConsole.cc
// Implementation of TclConsole.

#include "gui/kt_tclConsole.h"

#include "gui/kt_engine.h"
#include "tcl/kt_tclShell.h"

#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTextCharFormat>
#include <QVBoxLayout>
#include <algorithm>

namespace ktplace {

namespace {

const QString kPrompt = QStringLiteral("ktplace>");
const QString kContinue = QStringLiteral("      >");

}  // namespace

void ConsoleInput::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
        case Qt::Key_Up:
            if (onUp) {
                onUp();
            }
            return;
        case Qt::Key_Down:
            if (onDown) {
                onDown();
            }
            return;
        default:
            QLineEdit::keyPressEvent(event);
    }
}

// Tab is taken here, before QWidget::event turns it into a focus change. Not in
// focusNextPrevChild: Qt calls that on its own when it moves focus, which ran
// a completion nobody asked for.
bool ConsoleInput::event(QEvent *event) {
    if (event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Tab) {
        if (onTab) {
            onTab();
        }
        return true;
    }
    return QLineEdit::event(event);
}

TclConsole::TclConsole(Engine &engine, QWidget *parent) : QWidget(parent), engine_(engine) {
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);

    view_ = new QPlainTextEdit(this);
    view_->setReadOnly(true);
    view_->setFont(mono);
    view_->setMaximumBlockCount(20000);

    prompt_ = new QLabel(kPrompt, this);
    prompt_->setFont(mono);
    input_ = new ConsoleInput(this);
    input_->setFont(mono);
    input_->setPlaceholderText(QStringLiteral("Tcl command -- try `help`"));
    input_->onUp = [this] {
        historyStep(-1);
    };
    input_->onDown = [this] {
        historyStep(+1);
    };
    input_->onTab = [this] {
        complete();
    };
    connect(input_, &QLineEdit::returnPressed, this, [this] {
        submitLine();
    });

    auto *line = new QHBoxLayout;
    line->setContentsMargins(0, 0, 0, 0);
    line->addWidget(prompt_);
    line->addWidget(input_, 1);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(view_, 1);
    layout->addLayout(line);

    setFocusProxy(input_);
}

QString TclConsole::transcript() const {
    return view_->toPlainText();
}

void TclConsole::appendText(const QString &text, const QColor &color) {
    QTextCharFormat format;
    if (color.isValid()) {
        format.setForeground(color);
    }
    QTextCursor cursor(view_->document());
    cursor.movePosition(QTextCursor::End);
    if (!view_->document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(text, format);
    view_->ensureCursorVisible();
}

void TclConsole::submitLine() {
    if (busy_) {
        return;
    }
    const QString line = input_->text();
    input_->clear();
    appendText((pending_.isEmpty() ? kPrompt : kContinue) + " " + line);

    pending_ += pending_.isEmpty() ? line : "\n" + line;
    if (!TclShell::isComplete(pending_.toStdString())) {
        prompt_->setText(kContinue);
        return;
    }
    const QString script = pending_;
    pending_.clear();
    prompt_->setText(kPrompt);
    if (script.trimmed().isEmpty()) {
        return;
    }
    if (history_.isEmpty() || history_.last() != script) {
        history_ << script;
    }
    historyPos_ = static_cast<int>(history_.size());

    setBusy(true);
    engine_.submit(script, this, [this](const TclShell::Result &r) {
        const QString text = QString::fromStdString(r.ok ? r.text : r.trace);
        if (!text.isEmpty()) {
            appendText(text, r.ok ? QColor() : QColor(0xc0, 0x20, 0x20));
        }
        setBusy(false);
    });
}

void TclConsole::run(const QString &script) {
    if (busy_) {
        return;
    }
    // Whatever was half-typed is set aside, not mixed into the action.
    const QString typed = input_->text();
    pending_.clear();
    input_->setText(script);
    submitLine();
    input_->setText(typed);
}

void TclConsole::historyStep(int delta) {
    if (history_.isEmpty()) {
        return;
    }
    const int size = static_cast<int>(history_.size());
    historyPos_ = std::clamp(historyPos_ + delta, 0, size);
    // One past the end is the empty line the user was typing on.
    input_->setText(historyPos_ == size ? QString() : history_[historyPos_]);
}

void TclConsole::complete() {
    const QString text = input_->text();
    // Only the command word is completed: the text up to the first blank.
    if (text.isEmpty() || text.contains(' ')) {
        return;
    }
    QStringList matches;
    for (const QString &name : engine_.commandNames()) {
        if (name.startsWith(text)) {
            matches << name;
        }
    }
    if (matches.size() == 1) {
        input_->setText(matches.front() + " ");
        return;
    }
    if (matches.isEmpty()) {
        return;
    }
    // Extend to the longest common prefix, and list the candidates.
    QString common = matches.front();
    for (const QString &m : matches) {
        while (!m.startsWith(common)) {
            common.chop(1);
        }
    }
    input_->setText(common);
    appendText(matches.join("  "), palette().color(QPalette::PlaceholderText));
}

void TclConsole::setBusy(bool busy) {
    busy_ = busy;
    input_->setEnabled(!busy);
    prompt_->setText(busy ? QStringLiteral(" running")
                          : (pending_.isEmpty() ? kPrompt : kContinue));
    if (!busy) {
        input_->setFocus();
    }
    if (onBusyChanged) {
        onBusyChanged(busy);
    }
}

}  // namespace ktplace
