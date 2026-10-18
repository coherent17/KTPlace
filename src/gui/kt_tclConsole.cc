#include "gui/kt_tclConsole.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QStringList>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVBoxLayout>

namespace ktplace::gui {

namespace {

constexpr int maxLines = 20000;
const QColor commandColour(0xff, 0xd5, 0x4f);
const QColor errorColour(0xf2, 0x6b, 0x6b);

}  // namespace

class TclConsole::Impl {
public:
    QPlainTextEdit *output = nullptr;
    QLineEdit *input = nullptr;
    std::function<void(const QString &)> handler;
    QStringList history;
    qsizetype recall = 0;
};

TclConsole::TclConsole(QWidget *parent) : QWidget(parent), pImpl(std::make_unique<Impl>()) {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    pImpl->output = new QPlainTextEdit(this);
    pImpl->output->setReadOnly(true);
    pImpl->output->setMaximumBlockCount(maxLines);
    pImpl->output->setFont(fixed);
    pImpl->input = new QLineEdit(this);
    pImpl->input->setFont(fixed);
    pImpl->input->setPlaceholderText(
        QStringLiteral("Tcl: read_design <dir>, gplace_design, legalize_placement, detail_place, "
                       "write_placement, report_hpwl, source <file> ..."));
    pImpl->input->installEventFilter(this);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(2, 2, 2, 2);
    layout->addWidget(pImpl->output);
    layout->addWidget(pImpl->input);
    connect(pImpl->input, &QLineEdit::returnPressed, this, [this] {
        const QString command = pImpl->input->text().trimmed();
        pImpl->input->clear();
        submit(command);
    });
}

TclConsole::~TclConsole() = default;

void TclConsole::onCommand(std::function<void(const QString &)> handler) {
    pImpl->handler = std::move(handler);
}

void TclConsole::submit(const QString &command) {
    if (command.isEmpty()) {
        return;
    }
    if (pImpl->history.isEmpty() || pImpl->history.back() != command) {
        pImpl->history.append(command);
    }
    pImpl->recall = pImpl->history.size();
    append(QStringLiteral("% ") + command, Kind::Command);
    if (pImpl->handler) {
        pImpl->handler(command);
    }
}

void TclConsole::append(const QString &text, Kind kind) {
    QTextCharFormat format;
    if (kind == Kind::Command) {
        format.setForeground(commandColour);
    } else if (kind == Kind::Error) {
        format.setForeground(errorColour);
    }
    QTextCursor cursor(pImpl->output->document());
    cursor.movePosition(QTextCursor::End);
    if (!pImpl->output->document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(text, format);
    pImpl->output->ensureCursorVisible();
}

void TclConsole::setBusy(bool busy) {
    pImpl->input->setEnabled(!busy);
    if (!busy) {
        pImpl->input->setFocus();
    }
}

void TclConsole::focusInput() {
    pImpl->input->setFocus();
}

QString TclConsole::transcript() const {
    return pImpl->output->toPlainText();
}

bool TclConsole::eventFilter(QObject *watched, QEvent *event) {
    if (watched != pImpl->input || event->type() != QEvent::KeyPress) {
        return QWidget::eventFilter(watched, event);
    }
    const int key = static_cast<QKeyEvent *>(event)->key();
    if (key != Qt::Key_Up && key != Qt::Key_Down) {
        return QWidget::eventFilter(watched, event);
    }
    const qsizetype count = pImpl->history.size();
    pImpl->recall = key == Qt::Key_Up ? std::max<qsizetype>(0, pImpl->recall - 1)
                                      : std::min(count, pImpl->recall + 1);
    pImpl->input->setText(pImpl->recall < count ? pImpl->history[pImpl->recall] : QString());
    return true;
}

}  // namespace ktplace::gui
