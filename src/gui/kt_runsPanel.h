// @file kt_runsPanel.h
// The Runs panel: the benchmark designs and every run of them, in one tree.
//
//   Design / run          SimPL       NTUPlace1   Check   Time   Label
//   ISPD_2005/adaptec1    7.95e+07    8.40e+07                          best of each
//     2026-10-14 21:03    7.95e+07                legal   93.1s  tuned  a run
//       global_place      8.12e+07                        61.0s         its stages
//
// An algorithm column is "the HPWL with this algorithm" on every row: the best
// complete run on a design's row, the final one on a run's, the stage's on a
// stage's. The designs are those found under the benchmark root plus any the
// history has runs of, so a design opened from elsewhere is listed too.
//
// Double-click opens a design; the context menu runs the flow on it, and
// renames or deletes a run. The history is the window's (see kt_runHistory.h).

#pragma once

#include <QStringList>
#include <QWidget>
#include <functional>

class QLabel;
class QTreeWidget;

namespace ktplace {

class RunHistory;

class RunsPanel : public QWidget {
public:
    /// The algorithms with a column, in column order.
    static const QStringList &algorithms();

    explicit RunsPanel(RunHistory &history, QWidget *parent = nullptr);

    /// List the designs under @p root (rescanning) together with the history.
    void setRoot(const QString &root);
    /// Rebuild the tree from the history and the last scan.
    void refresh();

    bool renameRun(const QString &id, const QString &label);
    bool deleteRun(const QString &id);

    /// The design directories under @p root, relative to it and sorted. A
    /// directory is a design when it holds a .def or .nodes file (gzipped too).
    [[nodiscard]] static QStringList scan(const QString &root);
    [[nodiscard]] const QStringList &designs() const {
        return designs_;
    }
    [[nodiscard]] QTreeWidget *tree() const {
        return tree_;
    }

    /// Open a design (read_design), or run the flow on it with an algorithm.
    std::function<void(const QString &design)> onOpen;
    std::function<void(const QString &design, const QString &algorithm)> onRun;

private:
    void contextMenu(const QPoint &at);
    void exportTsv();

    RunHistory &history_;
    QString root_;
    QStringList designs_;  // relative to root_
    QTreeWidget *tree_ = nullptr;
    QLabel *where_ = nullptr;
};

}  // namespace ktplace
