#pragma once

#include "util/kt_log.h"

#include <QPlainTextEdit>

namespace ktplace::gui {

class LogView : public QPlainTextEdit {
public:
    explicit LogView(QWidget *parent = nullptr);
    ~LogView() override;

    LogView(const LogView &) = delete;
    LogView &operator=(const LogView &) = delete;

    void append(Logger::Level level, const QString &line);
};

}  // namespace ktplace::gui
