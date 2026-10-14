// @file kt_runsPanel.h
// The Runs panel: the run history as a table, newest first, one row per run
// with its stages underneath.
//
// A row shows when the run started, the design, the algorithm, the wirelength
// after global placement and at the end, the last placement check and the time
// spent. The panel only shows the history; what its actions do (open the
// design again, run the flow on it) is the window's, through the callbacks, so
// they go through the console like every other action. Renaming and deleting
// change the history itself, which is then saved.

#pragma once

#include <QString>
#include <QWidget>
#include <functional>

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

namespace ktplace {

class RunHistory;

class RunsPanel : public QWidget {
public:
    /// Shows @p history, which must outlive the panel.
    explicit RunsPanel(RunHistory &history, QWidget *parent = nullptr);

    /// Rebuild the table from the history, keeping the selection.
    void refresh();

    [[nodiscard]] QTreeWidget *tree() const {
        return tree_;
    }

    /// The run's id, for the selected row (or the run a stage row belongs to).
    [[nodiscard]] QString selectedRun() const;

    bool renameRun(const QString &id, const QString &label);
    bool deleteRun(const QString &id);

    /// Read the run's design again.
    std::function<void(const QString &design)> onOpen;
    /// Run the whole flow on the run's design with @p algorithm.
    std::function<void(const QString &design, const QString &algorithm)> onRerun;
    /// The history changed here (a rename or a delete).
    std::function<void()> onChanged;

private:
    void contextMenu(const QPoint &at);
    void exportTsv();
    void persist();

    RunHistory &history_;
    QTreeWidget *tree_ = nullptr;
    QLabel *where_ = nullptr;
};

}  // namespace ktplace
