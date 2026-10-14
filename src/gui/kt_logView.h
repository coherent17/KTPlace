// @file kt_logView.h
// The Log panel: every record the engine logs, as it is logged.
//
// Subscribes to ktlog as a sink (util/kt_log.h). Records arrive on whatever
// thread logged them -- the engine thread, or a TBB worker inside a stage -- and
// are queued to the GUI thread before they touch the widget. Trace records are
// dropped unless asked for, as they are on stderr.

#pragma once

#include "util/kt_log.h"

#include <QPlainTextEdit>

namespace ktplace {

class LogView : public QPlainTextEdit {
public:
    explicit LogView(QWidget *parent = nullptr);
    ~LogView() override;

    LogView(const LogView &) = delete;
    LogView &operator=(const LogView &) = delete;

    /// Also show trace records.
    void setShowTrace(bool show) {
        showTrace_ = show;
    }

    /// Append one record; GUI thread only. Public so the sink and tests share it.
    void appendRecord(LogLevel level, const QString &text);

private:
    Logger::SinkId sink_;
    bool showTrace_ = false;
};

}  // namespace ktplace
