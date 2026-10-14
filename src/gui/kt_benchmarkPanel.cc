// @file kt_benchmarkPanel.cc
// Implementation of BenchmarkPanel.

#include "gui/kt_benchmarkPanel.h"

#include "gui/kt_runHistory.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <optional>

namespace ktplace {

namespace {

constexpr int kPathRole = Qt::UserRole + 1;

// The design column, then a result and a baseline column per algorithm.
int resultColumn(int algorithm) {
    return 1 + 2 * algorithm;
}
int baselineColumn(int algorithm) {
    return 2 + 2 * algorithm;
}

constexpr int kSortRole = Qt::UserRole + 2;

// Sorts a result column by its number, not its text ("9e+07" < "1.2e+08"),
// with the designs that have no result last.
class Row : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;

    bool operator<(const QTreeWidgetItem &other) const override {
        const int col = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (col == 0) {
            return QTreeWidgetItem::operator<(other);
        }
        const QVariant a = data(col, kSortRole);
        const QVariant b = other.data(col, kSortRole);
        if (!a.isValid() || !b.isValid()) {
            return a.isValid();
        }
        return a.toDouble() < b.toDouble();
    }
};

}  // namespace

const QStringList &BenchmarkPanel::algorithms() {
    static const QStringList names{QStringLiteral("simpl"), QStringLiteral("ntuplace1")};
    return names;
}

BenchmarkPanel::BenchmarkPanel(const RunHistory &history, QWidget *parent)
    : QWidget(parent), history_(history) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(1 + 2 * static_cast<int>(algorithms().size()));
    tree_->setHeaderLabels({QStringLiteral("Design"), QStringLiteral("SimPL"),
                            QStringLiteral("baseline"), QStringLiteral("NTUPlace1"),
                            QStringLiteral("baseline")});
    tree_->setRootIsDecorated(false);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSortingEnabled(true);
    tree_->sortByColumn(0, Qt::AscendingOrder);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->setToolTip(
        QStringLiteral("Best final HPWL from the run history; double-click a "
                       "design to open it"));
    // The two kinds of column measure HPWL differently, so each header says how.
    for (int a = 0; a < static_cast<int>(algorithms().size()); ++a) {
        tree_->headerItem()->setToolTip(
            resultColumn(a),
            QStringLiteral("Best final HPWL of a complete run in the history, as the "
                           "database measures it (every pin, fixed ones included)"));
        tree_->headerItem()->setToolTip(
            baselineColumn(a),
            QStringLiteral("baseline.tsv: the HPWL in FastDP's own report, which leaves fixed "
                           "pins out of a net's vertical span and so reads lower than the "
                           "history's figure for the same placement"));
    }
    connect(tree_, &QWidget::customContextMenuRequested, this, &BenchmarkPanel::contextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (onOpen) {
            onOpen(item->data(0, kPathRole).toString());
        }
    });

    where_ = new QLabel(this);
    where_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    where_->setWordWrap(true);
    auto *choose = new QPushButton(QStringLiteral("Root..."), this);
    connect(choose, &QPushButton::clicked, this, &BenchmarkPanel::chooseRoot);
    auto *again = new QPushButton(QStringLiteral("Rescan"), this);
    connect(again, &QPushButton::clicked, this, &BenchmarkPanel::rescan);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(where_, 1);
    bar->addWidget(choose);
    bar->addWidget(again);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(tree_, 1);
    rescan();
}

bool BenchmarkPanel::looksLikeDesign(const QString &dir) {
    const QStringList markers{QStringLiteral("*.def"), QStringLiteral("*.def.gz"),
                              QStringLiteral("*.nodes"), QStringLiteral("*.nodes.gz")};
    return !QDir(dir).entryList(markers, QDir::Files).isEmpty();
}

QStringList BenchmarkPanel::scan(const QString &root) {
    QStringList found;
    const QDir base(root);
    if (root.isEmpty() || !base.exists()) {
        return found;
    }
    if (looksLikeDesign(root)) {
        found << QStringLiteral(".");
    }
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString dir = it.next();
        if (looksLikeDesign(dir)) {
            found << base.relativeFilePath(dir);
        }
    }
    found.sort();
    return found;
}

void BenchmarkPanel::setRoot(const QString &root) {
    root_ = root.isEmpty() ? QString() : QDir::cleanPath(QDir(root).absolutePath());
    rescan();
}

void BenchmarkPanel::chooseRoot() {
    const QString dir =
        QFileDialog::getExistingDirectory(this, QStringLiteral("Benchmark root"), root_);
    if (!dir.isEmpty()) {
        setRoot(dir);
    }
}

void BenchmarkPanel::readBaseline() {
    baseline_.clear();
    QFile in(QDir(root_).filePath(QStringLiteral("baseline.tsv")));
    if (root_.isEmpty() || !in.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream text(&in);
    QStringList header;
    while (!text.atEnd()) {
        const QString line = text.readLine();
        if (line.startsWith('#') || line.trimmed().isEmpty()) {
            continue;
        }
        const QStringList cells = line.split('\t');
        if (header.isEmpty()) {
            header = cells;
            continue;
        }
        const auto field = [&](const char *name) {
            const qsizetype i = header.indexOf(QString::fromLatin1(name));
            return i >= 0 && i < cells.size() ? cells[i] : QString();
        };
        bool ok = false;
        const double hpwl = field("hpwl").toDouble(&ok);
        if (ok && field("status") == QStringLiteral("done")) {
            baseline_.insert(field("design") + '\t' + field("algorithm"), {hpwl, field("verdict")});
        }
    }
}

void BenchmarkPanel::rescan() {
    designs_ = scan(root_);
    readBaseline();
    tree_->setSortingEnabled(false);
    tree_->clear();
    const QDir base(root_);
    for (const QString &design : designs_) {
        auto *row = new Row(tree_);
        row->setText(0, design);
        const QString path = QDir::cleanPath(base.filePath(design));
        row->setData(0, kPathRole, path);
        row->setToolTip(0, path);
    }
    tree_->setSortingEnabled(true);
    where_->setText(root_.isEmpty() ? QStringLiteral("No benchmark root")
                                    : QStringLiteral("%1 design(s) under %2")
                                          .arg(designs_.size())
                                          .arg(QDir::toNativeSeparators(root_)));
    refreshResults();
}

void BenchmarkPanel::refreshResults() {
    const QStringList &algos = algorithms();
    // Rows would re-sort as their sort column changes, under the loop's index.
    tree_->setSortingEnabled(false);
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem *row = tree_->topLevelItem(i);
        const QString path = row->data(0, kPathRole).toString();
        const QString design = row->text(0);
        int bestColumn = -1;
        double bestHpwl = 0.0;
        for (int a = 0; a < algos.size(); ++a) {
            const auto best = history_.best(path, algos[a]);
            const int col = resultColumn(a);
            QFont font = row->font(col);
            font.setBold(false);
            row->setFont(col, font);
            if (best) {
                row->setText(col, QString::number(best->hpwl, 'e', 4));
                row->setData(col, kSortRole, best->hpwl);
                row->setToolTip(
                    col, QStringLiteral("run %1, %2")
                             .arg(best->runId, best->verdict.isEmpty() ? QStringLiteral("unchecked")
                                                                       : best->verdict));
                row->setForeground(col, best->verdict == QStringLiteral("legal")
                                            ? palette().text().color()
                                            : QColor(0xd0, 0x50, 0x40));
                if (bestColumn < 0 || best->hpwl < bestHpwl) {
                    bestColumn = col;
                    bestHpwl = best->hpwl;
                }
            } else {
                row->setText(col, QString());
                row->setData(col, kSortRole, QVariant());
                row->setToolTip(col, QString());
            }
            const auto base = baseline_.constFind(design + '\t' + algos[a]);
            if (base != baseline_.constEnd()) {
                row->setText(baselineColumn(a), QString::number(base->first, 'e', 4));
                row->setData(baselineColumn(a), kSortRole, base->first);
                row->setToolTip(baselineColumn(a),
                                QStringLiteral("baseline.tsv, verdict %1").arg(base->second));
            } else {
                row->setText(baselineColumn(a), QString());
                row->setData(baselineColumn(a), kSortRole, QVariant());
            }
        }
        if (bestColumn >= 0) {
            QFont font = row->font(bestColumn);
            font.setBold(true);
            row->setFont(bestColumn, font);
        }
    }
    tree_->setSortingEnabled(true);
    for (int c = 0; c < tree_->columnCount(); ++c) {
        tree_->resizeColumnToContents(c);
    }
}

void BenchmarkPanel::contextMenu(const QPoint &at) {
    QTreeWidgetItem *item = tree_->itemAt(at);
    if (!item) {
        return;
    }
    const QString path = item->data(0, kPathRole).toString();
    QMenu menu(this);
    QAction *open = menu.addAction(QStringLiteral("Open Design"));
    open->setEnabled(static_cast<bool>(onOpen));
    menu.addSeparator();
    QList<QAction *> runs;
    for (const QString &algo : algorithms()) {
        QAction *run = menu.addAction(QStringLiteral("Run Flow (%1)").arg(algo));
        run->setData(algo);
        run->setEnabled(static_cast<bool>(onRun));
        runs << run;
    }
    QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(at));
    if (chosen == open) {
        onOpen(path);
    } else if (runs.contains(chosen)) {
        onRun(path, chosen->data().toString());
    }
}

}  // namespace ktplace
