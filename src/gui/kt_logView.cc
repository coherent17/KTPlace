#include "gui/kt_logView.h"

#include <QFontDatabase>
#include <QMetaObject>
#include <QTextCharFormat>
#include <QTextCursor>
#include <string_view>

namespace ktplace::gui {

namespace {

constexpr int maxLines = 50000;

}  // namespace

LogView::LogView(QWidget *parent) : QPlainTextEdit(parent) {
    setReadOnly(true);
    setMaximumBlockCount(maxLines);
    setLineWrapMode(QPlainTextEdit::NoWrap);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    ktlog.setSink([this](Logger::Level level, std::string_view text) {
        if (level == Logger::Level::Trace) {
            return;
        }
        const QString line = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
        QMetaObject::invokeMethod(
            this,
            [this, level, line] {
                append(level, line);
            },
            Qt::QueuedConnection);
    });
}

LogView::~LogView() {
    ktlog.setSink(nullptr);
}

void LogView::append(Logger::Level level, const QString &line) {
    QTextCharFormat format;
    if (level == Logger::Level::Warning) {
        format.setForeground(QColor(0xc0, 0x7a, 0x00));
    } else if (level == Logger::Level::Fatal) {
        format.setForeground(QColor(0xc0, 0x20, 0x20));
        format.setFontWeight(QFont::Bold);
    }
    QTextCursor cursor(document());
    cursor.movePosition(QTextCursor::End);
    if (!document()->isEmpty()) {
        cursor.insertBlock();
    }
    cursor.insertText(line, format);
    ensureCursorVisible();
}

}  // namespace ktplace::gui
