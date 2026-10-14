// @file kt_runsPanel.cc
// Implementation of RunsPanel.

#include "gui/kt_runsPanel.h"

#include "gui/kt_runHistory.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace ktplace {

namespace {

enum Column { kName, kFirstAlgorithm, kCheck = 3, kTime, kLabel, kColumns };
constexpr int kPathRole = Qt::UserRole + 1;  // a design's directory, on every row
constexpr int kIdRole = Qt::UserRole + 2;    // a run's id, on its rows

QString canonical(const QString &dir) {
    return QDir::cleanPath(QDir(dir).absolutePath());
}

QString seconds(double s) {
    return s < 60.0 ? QString::number(s, 'f', 1) + 's'
                    : QStringLiteral("%1m %2s")
                          .arg(static_cast<int>(s / 60.0))
                          .arg(static_cast<int>(s) % 60, 2, 10, QChar('0'));
}

int columnOf(const QString &algorithm) {
    const qsizetype i = RunsPanel::algorithms().indexOf(algorithm);
    return i < 0 ? -1 : kFirstAlgorithm + static_cast<int>(i);
}

void setHpwl(QTreeWidgetItem *row, int column, double hpwl) {
    if (column >= 0) {
        row->setText(column, QString::number(hpwl, 'e', 4));
    }
}

}  // namespace

const QStringList &RunsPanel::algorithms() {
    static const QStringList names{QStringLiteral("simpl"), QStringLiteral("ntuplace1")};
    return names;
}

RunsPanel::RunsPanel(RunHistory &history, QWidget *parent) : QWidget(parent), history_(history) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(kColumns);
    tree_->setHeaderLabels({QStringLiteral("Design / run"), QStringLiteral("SimPL"),
                            QStringLiteral("NTUPlace1"), QStringLiteral("Check"),
                            QStringLiteral("Time"), QStringLiteral("Label")});
    tree_->headerItem()->setToolTip(
        kFirstAlgorithm, QStringLiteral("Final HPWL: the best complete run on a design's row"));
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setStretchLastSection(true);
    connect(tree_, &QWidget::customContextMenuRequested, this, &RunsPanel::contextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (onOpen) {
            onOpen(item->data(0, kPathRole).toString());
        }
    });

    where_ = new QLabel(this);
    where_->setWordWrap(true);
    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(where_, 1);
    const auto button = [&](const char *text, auto slot) {
        auto *b = new QPushButton(QString::fromLatin1(text), this);
        connect(b, &QPushButton::clicked, this, slot);
        bar->addWidget(b);
    };
    button("Root...", [this] {
        const QString dir =
            QFileDialog::getExistingDirectory(this, QStringLiteral("Benchmark root"), root_);
        if (!dir.isEmpty()) {
            setRoot(dir);
        }
    });
    button("Rescan", [this] {
        setRoot(root_);
    });
    button("Export TSV...", [this] {
        exportTsv();
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(tree_, 1);
    refresh();
}

QStringList RunsPanel::scan(const QString &root) {
    QStringList found;
    if (root.isEmpty() || !QDir(root).exists()) {
        return found;
    }
    const QStringList markers{QStringLiteral("*.def"), QStringLiteral("*.def.gz"),
                              QStringLiteral("*.nodes"), QStringLiteral("*.nodes.gz")};
    const auto isDesign = [&](const QString &dir) {
        return !QDir(dir).entryList(markers, QDir::Files).isEmpty();
    };
    if (isDesign(root)) {
        found << QStringLiteral(".");
    }
    QDirIterator it(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (const QString dir = it.next(); isDesign(dir)) {
            found << QDir(root).relativeFilePath(dir);
        }
    }
    found.sort();
    return found;
}

void RunsPanel::setRoot(const QString &root) {
    root_ = root.isEmpty() ? QString() : canonical(root);
    designs_ = scan(root_);
    refresh();
}

void RunsPanel::refresh() {
    // One row per design, keyed by its canonical path: the scanned ones and
    // any the history has runs of, in name order. Only the design rows are
    // sorted; runs stay newest first under them.
    const auto &runs = history_.runs();
    const auto display = [this](const QString &key) {
        const QString rel = QDir(root_).relativeFilePath(key);
        return root_.isEmpty() || rel.startsWith("..") ? key : rel;
    };
    QStringList keys;
    for (const QString &design : designs_) {
        keys << canonical(QDir(root_).filePath(design));
    }
    for (const RunRecord &r : runs) {
        keys << canonical(r.design);
    }
    keys.removeDuplicates();
    std::ranges::sort(keys, [&](const QString &a, const QString &b) {
        return display(a) < display(b);
    });
    tree_->clear();
    QMap<QString, QTreeWidgetItem *> rows;
    for (const QString &key : keys) {
        auto *row = new QTreeWidgetItem(tree_);
        row->setText(kName, display(key));
        row->setToolTip(kName, key);
        row->setData(0, kPathRole, key);
        rows.insert(key, row);
    }

    for (auto r = runs.rbegin(); r != runs.rend(); ++r) {  // newest first
        QTreeWidgetItem *design = rows.value(canonical(r->design));
        auto *run = new QTreeWidgetItem(design);
        run->setData(0, kPathRole, design->data(0, kPathRole));
        run->setData(0, kIdRole, r->id);
        run->setText(kName, r->started.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        const int column = columnOf(r->algorithm());
        if (const auto hpwl = r->finalHpwl()) {
            setHpwl(run, column, *hpwl);
        }
        QString check = r->verdict();
        if (r->stopped()) {
            check += check.isEmpty() ? QStringLiteral("stopped") : QStringLiteral(", stopped");
        }
        run->setText(kCheck, check);
        if (!r->verdict().isEmpty()) {
            run->setForeground(kCheck, r->verdict() == QStringLiteral("legal")
                                           ? QColor(0x3c, 0xa0, 0x50)
                                           : QColor(0xd0, 0x50, 0x40));
        }
        run->setText(kTime, seconds(r->seconds()));
        run->setText(kLabel, r->label);
        for (const RunStage &s : r->stages) {
            auto *stage = new QTreeWidgetItem(run);
            stage->setData(0, kPathRole, design->data(0, kPathRole));
            stage->setData(0, kIdRole, r->id);
            stage->setText(kName, s.detail.isEmpty() ? s.stage : s.stage + ' ' + s.detail);
            setHpwl(stage, column, s.hpwl);
            stage->setText(kCheck, s.stopped ? QStringLiteral("stopped") : QString());
            stage->setText(kTime, seconds(s.seconds));
        }
    }
    // The best complete run per algorithm, on the design's row; the better of
    // the two in bold.
    for (QTreeWidgetItem *row : rows) {
        int boldColumn = -1;
        double boldHpwl = 0.0;
        for (const QString &algorithm : algorithms()) {
            const auto best = history_.best(row->data(0, kPathRole).toString(), algorithm);
            if (!best) {
                continue;
            }
            const int column = columnOf(algorithm);
            setHpwl(row, column, best->hpwl);
            row->setToolTip(column, QStringLiteral("best complete run, %1")
                                        .arg(best->verdict.isEmpty() ? QStringLiteral("unchecked")
                                                                     : best->verdict));
            if (boldColumn < 0 || best->hpwl < boldHpwl) {
                boldColumn = column;
                boldHpwl = best->hpwl;
            }
        }
        if (boldColumn >= 0) {
            QFont font = row->font(boldColumn);
            font.setBold(true);
            row->setFont(boldColumn, font);
        }
    }
    for (int c = 0; c < kLabel; ++c) {
        tree_->resizeColumnToContents(c);
    }
    where_->setText(
        QStringLiteral("%1 design(s)%2, %3 run(s)%4")
            .arg(rows.size())
            .arg(root_.isEmpty() ? QString() : " under " + QDir::toNativeSeparators(root_))
            .arg(runs.size())
            .arg(history_.file().isEmpty() ? QStringLiteral(", not saved")
                                           : " in " + QDir::toNativeSeparators(history_.file())));
}

bool RunsPanel::renameRun(const QString &id, const QString &label) {
    if (!history_.rename(id, label)) {
        return false;
    }
    history_.save();
    refresh();
    return true;
}

bool RunsPanel::deleteRun(const QString &id) {
    if (!history_.remove(id)) {
        return false;
    }
    history_.save();
    refresh();
    return true;
}

void RunsPanel::contextMenu(const QPoint &at) {
    const QTreeWidgetItem *item = tree_->itemAt(at);
    if (!item) {
        return;
    }
    // Copied out: the menu runs an event loop, and a script finishing in it
    // refreshes the tree under the item.
    const QString design = item->data(0, kPathRole).toString();
    const QString id = item->data(0, kIdRole).toString();
    const RunRecord *run = history_.find(id);
    const QString label = run ? run->label : QString();

    QMenu menu(this);
    QAction *open = menu.addAction(QStringLiteral("Open Design"));
    menu.addSeparator();
    for (const QString &algorithm : algorithms()) {
        menu.addAction(QStringLiteral("Run Flow (%1)").arg(algorithm))->setData(algorithm);
    }
    QAction *rename = nullptr;
    QAction *remove = nullptr;
    if (run) {
        menu.addSeparator();
        rename = menu.addAction(QStringLiteral("Rename Run..."));
        remove = menu.addAction(QStringLiteral("Delete Run"));
    }
    QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(at));
    if (!chosen) {
        return;
    }
    if (chosen == open && onOpen) {
        onOpen(design);
    } else if (chosen == rename) {
        bool ok = false;
        const QString text =
            QInputDialog::getText(this, QStringLiteral("Rename run"), QStringLiteral("Label:"),
                                  QLineEdit::Normal, label, &ok);
        if (ok) {
            renameRun(id, text.trimmed());
        }
    } else if (chosen == remove) {
        deleteRun(id);
    } else if (chosen->data().isValid() && onRun) {
        onRun(design, chosen->data().toString());
    }
}

void RunsPanel::exportTsv() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export run history"), QStringLiteral("runs.tsv"),
        QStringLiteral("Tab-separated values (*.tsv);;All files (*)"));
    QFile out(path);
    if (!path.isEmpty() && out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        out.write(history_.toTsv().toUtf8());
    }
}

}  // namespace ktplace
