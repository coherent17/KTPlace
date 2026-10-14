// @file kt_runsPanel.cc
// Implementation of RunsPanel.

#include "gui/kt_runsPanel.h"

#include "gui/kt_runHistory.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace ktplace {

namespace {

enum Column { kStarted, kDesign, kAlgorithm, kGlobal, kFinal, kCheck, kTime, kLabel, kColumns };

constexpr int kIdRole = Qt::UserRole + 1;

QString hpwlText(const std::optional<double> &v) {
    return v ? QString::number(*v, 'e', 4) : QString();
}

QString seconds(double s) {
    return s < 60.0 ? QString::number(s, 'f', 1) + 's'
                    : QStringLiteral("%1m %2s")
                          .arg(static_cast<int>(s / 60.0))
                          .arg(static_cast<int>(s) % 60, 2, 10, QChar('0'));
}

}  // namespace

RunsPanel::RunsPanel(RunHistory &history, QWidget *parent) : QWidget(parent), history_(history) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(kColumns);
    tree_->setHeaderLabels({QStringLiteral("Started"), QStringLiteral("Design"),
                            QStringLiteral("Algorithm"), QStringLiteral("Global HPWL"),
                            QStringLiteral("Final HPWL"), QStringLiteral("Check"),
                            QStringLiteral("Time"), QStringLiteral("Label")});
    tree_->setRootIsDecorated(true);
    tree_->setUniformRowHeights(true);
    tree_->setAlternatingRowColors(true);
    tree_->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    tree_->header()->setStretchLastSection(true);
    connect(tree_, &QWidget::customContextMenuRequested, this, &RunsPanel::contextMenu);
    connect(tree_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        const RunRecord *r = history_.find(item->data(0, kIdRole).toString());
        if (r && onOpen) {
            onOpen(r->design);
        }
    });

    where_ = new QLabel(this);
    where_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *exportButton = new QPushButton(QStringLiteral("Export TSV..."), this);
    connect(exportButton, &QPushButton::clicked, this, &RunsPanel::exportTsv);

    auto *bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(where_, 1);
    bar->addWidget(exportButton);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addLayout(bar);
    layout->addWidget(tree_, 1);

    refresh();
}

void RunsPanel::refresh() {
    const QString keep = selectedRun();
    tree_->clear();
    const auto &runs = history_.runs();
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        const RunRecord &r = *it;
        auto *row = new QTreeWidgetItem(tree_);
        row->setData(0, kIdRole, r.id);
        row->setText(kStarted, r.started.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
        row->setText(kDesign, QFileInfo(r.design).fileName());
        row->setToolTip(kDesign, r.design);
        row->setText(kAlgorithm, r.algorithm());
        row->setText(kGlobal, hpwlText(r.globalHpwl()));
        row->setText(kFinal, hpwlText(r.finalHpwl()));
        QString check = r.verdict();
        if (r.stopped()) {
            check =
                check.isEmpty() ? QStringLiteral("stopped") : check + QStringLiteral(", stopped");
        }
        row->setText(kCheck, check);
        if (r.verdict() == QStringLiteral("legal")) {
            row->setForeground(kCheck, QColor(0x3c, 0xa0, 0x50));
        } else if (!r.verdict().isEmpty()) {
            row->setForeground(kCheck, QColor(0xd0, 0x50, 0x40));
        }
        row->setText(kTime, seconds(r.seconds()));
        row->setText(kLabel, r.label);
        for (const RunStage &s : r.stages) {
            auto *child = new QTreeWidgetItem(row);
            child->setData(0, kIdRole, r.id);
            child->setText(kStarted, s.stage);
            child->setText(kAlgorithm, s.detail);
            child->setText(kFinal, QString::number(s.hpwl, 'e', 4));
            child->setText(kCheck, s.stopped ? QStringLiteral("stopped") : QString());
            child->setText(kTime, seconds(s.seconds));
        }
        if (r.id == keep) {
            tree_->setCurrentItem(row);
        }
    }
    for (int c = 0; c < kLabel; ++c) {
        tree_->resizeColumnToContents(c);
    }
    where_->setText(history_.file().isEmpty()
                        ? QStringLiteral("%1 run(s), not saved").arg(runs.size())
                        : QStringLiteral("%1 run(s) in %2")
                              .arg(runs.size())
                              .arg(QDir::toNativeSeparators(history_.file())));
}

QString RunsPanel::selectedRun() const {
    const QTreeWidgetItem *item = tree_->currentItem();
    return item ? item->data(0, kIdRole).toString() : QString();
}

bool RunsPanel::renameRun(const QString &id, const QString &label) {
    if (!history_.rename(id, label)) {
        return false;
    }
    persist();
    return true;
}

bool RunsPanel::deleteRun(const QString &id) {
    if (!history_.remove(id)) {
        return false;
    }
    persist();
    return true;
}

void RunsPanel::persist() {
    history_.save();
    refresh();
    if (onChanged) {
        onChanged();
    }
}

void RunsPanel::contextMenu(const QPoint &at) {
    QTreeWidgetItem *item = tree_->itemAt(at);
    if (!item) {
        return;
    }
    tree_->setCurrentItem(item);
    const RunRecord *r = history_.find(item->data(0, kIdRole).toString());
    if (!r) {
        return;
    }
    // Copied out: the menu runs an event loop, and a script finishing in it
    // updates the history, which can move the record.
    const QString id = r->id;
    const QString design = r->design;
    const QString label = r->label;
    const QString algorithm = r->algorithm().isEmpty() ? QStringLiteral("simpl") : r->algorithm();

    QMenu menu(this);
    QAction *open = menu.addAction(QStringLiteral("Open Design"));
    QAction *rerun = menu.addAction(QStringLiteral("Run Flow Again (%1)").arg(algorithm));
    menu.addSeparator();
    QAction *rename = menu.addAction(QStringLiteral("Rename..."));
    QAction *remove = menu.addAction(QStringLiteral("Delete"));
    open->setEnabled(static_cast<bool>(onOpen));
    rerun->setEnabled(static_cast<bool>(onRerun));

    QAction *chosen = menu.exec(tree_->viewport()->mapToGlobal(at));
    if (chosen == open) {
        onOpen(design);
    } else if (chosen == rerun) {
        onRerun(design, algorithm);
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
    }
}

void RunsPanel::exportTsv() {
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("Export run history"), QStringLiteral("runs.tsv"),
        QStringLiteral("Tab-separated values (*.tsv);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, QStringLiteral("Export run history"),
                             QStringLiteral("Cannot write %1").arg(path));
        return;
    }
    out.write(history_.toTsv().toUtf8());
}

}  // namespace ktplace
