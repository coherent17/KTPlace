// @file kt_logView.cc
// Implementation of LogView.

#include "gui/kt_logView.h"

#include <QFontDatabase>
#include <QMetaObject>
#include <QTextCharFormat>
#include <string_view>

namespace ktplace {

namespace {

// Enough to scroll back through a large design's whole run, few enough that
// appending stays cheap: past this the oldest lines are dropped.
constexpr int kMaxLines = 50000;

}  // namespace

LogView::LogView(QWidget *parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(kMaxLines);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    // Called on the logging thread, so it only copies the text and queues it.
    // `this` is alive for every call: the destructor's removeSink runs first and
    // waits out a call in flight. A queued call still undelivered when the view
    // is destroyed is dropped by Qt along with the view's other posted events.
    sink_ = ktlog.addSink([this](LogLevel level, std::string_view text) {
        const QString line = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        QMetaObject::invokeMethod(
            this,
            [this, level, line] {
                appendRecord(level, line);
            },
            Qt::QueuedConnection);
    });
}

LogView::~LogView() {
    // removeSink waits for a call in flight, so no sink call outlives this.
    ktlog.removeSink(sink_);
}

void LogView::appendRecord(LogLevel level, const QString &text) {
    if (level == LogLevel::Trace && !showTrace_) {
        return;
    }
    QTextCharFormat format;
    switch (level) {
        case LogLevel::Warning:
            format.setForeground(QColor(0xc0, 0x7a, 0x00));
            break;
        case LogLevel::Fatal:
            format.setForeground(QColor(0xc0, 0x20, 0x20));
            format.setFontWeight(QFont::Bold);
            break;
        case LogLevel::Trace:
            format.setForeground(palette().color(QPalette::PlaceholderText));
            break;
        case LogLevel::Echo:
            break;
    }
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (!document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(text, format);
    ensureCursorVisible();
}

}  // namespace ktplace
