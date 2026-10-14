// @file kt_benchmarkPanel.h
// The Benchmarks panel: every design under the benchmark root, with the best
// result each algorithm has reached on it.
//
// A directory is a design when it holds a .def or a .nodes file (gzipped or
// not), the same test the readers use to recognise one. Each row shows the
// shortest final wirelength the run history has for SimPL and for NTUPlace1,
// the better one in bold, and -- when the root carries a baseline.tsv (see
// scripts/bench_baseline.py) -- the recorded baseline next to it, so a change
// to the engine can be judged against the last sweep. The two are not measured
// alike: the history keeps the database's HPWL, the baseline FastDP's, which
// leaves fixed pins out of the vertical span and reads lower (1.064e7 against
// 1.194e7 for one ibm01 placement). The column headers say so.
//
// Double-click a design to read it; the context menu also runs the whole flow
// on it with either algorithm. Both go through the window's callbacks, so they
// are console commands like any other action.

#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <functional>

class QLabel;
class QTreeWidget;

namespace ktplace {

class RunHistory;

class BenchmarkPanel : public QWidget {
public:
    /// The algorithms with a column each, in order.
    static const QStringList &algorithms();

    /// Results come from @p history, which must outlive the panel.
    explicit BenchmarkPanel(const RunHistory &history, QWidget *parent = nullptr);

    /// Look for designs under @p root (and read its baseline.tsv).
    void setRoot(const QString &root);
    [[nodiscard]] const QString &root() const {
        return root_;
    }
    /// Scan the root again.
    void rescan();
    /// Redraw the result columns from the history; the designs stay.
    void refreshResults();

    /// The design directories found, relative to the root, sorted.
    [[nodiscard]] const QStringList &designs() const {
        return designs_;
    }
    [[nodiscard]] QTreeWidget *tree() const {
        return tree_;
    }

    /// Every design directory under @p root, relative to it, sorted.
    [[nodiscard]] static QStringList scan(const QString &root);
    /// Whether @p dir directly holds a design's files.
    [[nodiscard]] static bool looksLikeDesign(const QString &dir);

    /// Read the design at @p path (absolute).
    std::function<void(const QString &path)> onOpen;
    /// Run the whole flow on the design at @p path with @p algorithm.
    std::function<void(const QString &path, const QString &algorithm)> onRun;

private:
    void contextMenu(const QPoint &at);
    void readBaseline();
    void chooseRoot();

    const RunHistory &history_;
    QString root_;
    QStringList designs_;
    /// "design\talgorithm" -> the baseline's HPWL and verdict.
    QHash<QString, QPair<double, QString>> baseline_;
    QTreeWidget *tree_ = nullptr;
    QLabel *where_ = nullptr;
};

}  // namespace ktplace
