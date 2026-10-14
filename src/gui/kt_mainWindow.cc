// @file kt_mainWindow.cc
// Implementation of MainWindow.

#include "gui/kt_mainWindow.h"

#include "kt_option.h"

#include "gui/kt_engine.h"
#include "gui/kt_layoutSnapshot.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_metricsView.h"
#include "gui/kt_propertyPanel.h"
#include "gui/kt_runsPanel.h"
#include "gui/kt_tclConsole.h"
#include "session/kt_session.h"
#include "util/kt_log.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QToolBar>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

enum Dialog { kOpenDirectory, kOpenFile, kSaveFile };
// docks_, in the order the constructor adds them.
enum Dock { kPropertiesDock, kLogDock, kConsoleDock, kMetricsDock, kRunsDock };

}  // namespace

MainWindow::MainWindow(Engine &engine, QWidget *parent) : QMainWindow(parent), engine_(engine) {
    setWindowTitle(QStringLiteral("KTPlace"));
    resize(1280, 860);

    layout_ = new LayoutView(this);
    layout_->onSelect = [this](std::optional<std::size_t> cell) {
        inspect(cell);
    };
    layout_->onCursor = [this](std::optional<QPointF> at) {
        cursor_->setText(
            at ? QStringLiteral("x %1  y %2").arg(at->x(), 0, 'f', 1).arg(at->y(), 0, 'f', 1)
               : QString());
    };
    setCentralWidget(layout_);

    properties_ = new PropertyPanel(this);
    log_ = new LogView(this);
    console_ = new TclConsole(engine_, this);
    console_->onBusyChanged = [this](bool busy) {
        setBusy(busy);
    };
    metrics_ = new MetricsView(this);
    runs_ = new RunsPanel(history_, this);
    runs_->onOpen = [this](const QString &design) {
        console_->run("read_design " + tclQuote(design));
    };
    runs_->onRun = [this](const QString &design, const QString &algorithm) {
        runFlowOn(design, algorithm);
    };

    QDockWidget *props =
        addDock(QStringLiteral("Properties"), properties_, Qt::RightDockWidgetArea);
    resizeDocks({props}, {280}, Qt::Horizontal);
    QDockWidget *bottom = addDock(QStringLiteral("Log"), log_, Qt::BottomDockWidgetArea);
    for (auto [title, widget] : {std::pair<const char *, QWidget *>{"Tcl Console", console_},
                                 {"Metrics", metrics_},
                                 {"Runs", runs_}}) {
        QDockWidget *dock = addDock(QString::fromLatin1(title), widget, Qt::BottomDockWidgetArea);
        tabifyDockWidget(bottom, dock);
    }
    docks_[kConsoleDock]->raise();
    resizeDocks({docks_[kConsoleDock]}, {300}, Qt::Vertical);

    status_ = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(status_, 1);
    cursor_ = new QLabel(this);
    cursor_->setMinimumWidth(200);
    statusBar()->addPermanentWidget(cursor_);

    buildMenus();
    engine_.watchProgress(this, [this](const Engine::LiveUpdate &update) {
        showProgress(update.sample, update.layout);
    });
    console_->setFocus();
}

QDockWidget *MainWindow::addDock(const QString &title, QWidget *widget, Qt::DockWidgetArea area) {
    auto *dock = new QDockWidget(title, this);
    dock->setObjectName(title);
    dock->setWidget(widget);
    addDockWidget(area, dock);
    docks_ << dock;
    return dock;
}

QString MainWindow::tclQuote(const QString &word) {
    // Braces keep everything literal, unless the word has braces of its own or
    // ends in a backslash; then every special character is escaped instead.
    if (!word.contains('{') && !word.contains('}') && !word.endsWith('\\')) {
        return '{' + word + '}';
    }
    QString out;
    for (const QChar c : word) {
        if (QStringLiteral("{}[]$\"\\; \t").contains(c)) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

QAction *MainWindow::engineAction(QMenu *menu, const QString &text, const QString &script,
                                  const QKeySequence &shortcut) {
    QAction *action = menu->addAction(text);
    action->setShortcut(shortcut);
    connect(action, &QAction::triggered, this, [this, script] {
        console_->run(script);
    });
    engineActions_ << action;
    return action;
}

void MainWindow::pathAction(QMenu *menu, const QString &text, const QString &command,
                            const QKeySequence &shortcut, int dialog) {
    QAction *action = menu->addAction(text);
    action->setShortcut(shortcut);
    connect(action, &QAction::triggered, this, [this, text, command, dialog] {
        const QString title = QString(text).remove('&').remove(QStringLiteral("..."));
        const QString path =
            dialog == kOpenDirectory ? QFileDialog::getExistingDirectory(this, title)
            : dialog == kOpenFile
                ? QFileDialog::getOpenFileName(this, title, {},
                                               QStringLiteral("Tcl scripts (*.tcl);;All files (*)"))
                : QFileDialog::getSaveFileName(
                      this, title, QStringLiteral("placed.pl"),
                      QStringLiteral("Bookshelf placement (*.pl);;All files (*)"));
        if (!path.isEmpty()) {
            console_->run(command + ' ' + tclQuote(path));
        }
    });
    engineActions_ << action;
}

void MainWindow::buildMenus() {
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    pathAction(file, QStringLiteral("&Open Design..."), QStringLiteral("read_design"),
               QKeySequence::Open, kOpenDirectory);
    pathAction(file, QStringLiteral("&Source Script..."), QStringLiteral("source"), {}, kOpenFile);
    pathAction(file, QStringLiteral("&Write Placement..."), QStringLiteral("write_design"),
               QKeySequence::Save, kSaveFile);
    file->addSeparator();
    connect(file->addAction(QStringLiteral("&Quit")), &QAction::triggered, this, &QWidget::close);
    file->actions().last()->setShortcut(QKeySequence::Quit);

    QMenu *place = menuBar()->addMenu(QStringLiteral("&Place"));
    QAction *global = engineAction(place, QStringLiteral("&Global Place"),
                                   QStringLiteral("global_place"), Qt::Key_F5);
    engineAction(place, QStringLiteral("Global Place (&NTUPlace1)"),
                 QStringLiteral("global_place -algo ntuplace1"));
    QAction *legalize =
        engineAction(place, QStringLiteral("&Legalize"), QStringLiteral("legalize"), Qt::Key_F6);
    QAction *detail = engineAction(place, QStringLiteral("&Detail Place"),
                                   QStringLiteral("detail_place"), Qt::Key_F7);
    place->addSeparator();
    QAction *flow = engineAction(place, QStringLiteral("Run &Full Flow"),
                                 flowScript(QStringLiteral("simpl")), Qt::Key_F9);
    place->addSeparator();
    // Not an engine action: it is the one thing to do while the engine is busy.
    stop_ = place->addAction(QStringLiteral("&Stop"));
    stop_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Period));
    stop_->setToolTip(
        QStringLiteral("End the running stage with the best placement it has, "
                       "and the rest of the script (Ctrl+.)"));
    stop_->setEnabled(false);
    connect(stop_, &QAction::triggered, this, [this] {
        if (console_->busy()) {
            ktlog.echo("stop requested");
            engine_.requestStop();
            status_->setText(QStringLiteral("Stopping..."));
        }
    });

    QMenu *report = menuBar()->addMenu(QStringLiteral("&Report"));
    for (auto [text, command] : {std::pair<const char *, const char *>{"&Design", "report_design"},
                                 {"&HPWL", "report_hpwl"},
                                 {"&Check Placement", "check_placement"},
                                 {"&Timing", "report_timing"},
                                 {"&Options", "report_options"}}) {
        engineAction(report, QString::fromLatin1(text), QString::fromLatin1(command));
    }

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    QAction *fit = view->addAction(QStringLiteral("Zoom &Fit"));
    fit->setShortcut(Qt::Key_F);
    fit->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    layout_->addAction(fit);
    connect(fit, &QAction::triggered, this, [this] {
        layout_->zoomFit();
    });
    view->addSeparator();
    for (QDockWidget *dock : docks_) {
        view->addAction(dock->toggleViewAction());
    }
    QAction *trace = view->addAction(QStringLiteral("Show &Trace in Log"));
    trace->setCheckable(true);
    connect(trace, &QAction::toggled, this, [this](bool on) {
        log_->setShowTrace(on);
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    engineAction(help, QStringLiteral("&Commands"), QStringLiteral("help"));
    connect(help->addAction(QStringLiteral("&About KTPlace")), &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About KTPlace"),
            QStringLiteral("KTPlace v%1\nKnow Thyself Placement Engine")
                .arg(QString::fromUtf8(VERSION.data(), static_cast<qsizetype>(VERSION.size()))));
    });

    // The toolbar shows the same actions, by short names.
    QToolBar *bar = addToolBar(QStringLiteral("Flow"));
    bar->setObjectName(QStringLiteral("flowToolBar"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    for (auto [action, name] : {std::pair<QAction *, const char *>{flow, "Run Flow"},
                                {stop_, nullptr},
                                {global, "Global"},
                                {legalize, "Legalize"},
                                {detail, "Detail"},
                                {fit, nullptr},
                                {docks_[kMetricsDock]->toggleViewAction(), nullptr}}) {
        if (name) {
            action->setIconText(QString::fromLatin1(name));
        }
        bar->addAction(action);
    }
}

QString MainWindow::flowScript(const QString &algorithm) {
    // The fixed flow's stages, in one script: scripts/flow.tcl without the I/O.
    const QString global = algorithm == QStringLiteral("simpl")
                               ? QStringLiteral("global_place")
                               : QStringLiteral("global_place -algo ") + algorithm;
    return global + QStringLiteral(
                        "; legalize; check_placement {after legalization}; "
                        "detail_place; check_placement {after detail placement}; "
                        "report_hpwl");
}

void MainWindow::runFlowOn(const QString &design, const QString &algorithm) {
    console_->run("read_design " + tclQuote(design) + "; " + flowScript(algorithm));
}

void MainWindow::setHistoryFile(const QString &file) {
    history_ = RunHistory(file);
    if (!history_.load()) {
        ktlog.warning("cannot read the run history {}; starting a new one", file.toStdString());
    }
    runs_->refresh();
}

void MainWindow::setBenchmarkRoot(const QString &root) {
    runs_->setRoot(root);
}

void MainWindow::recordRun() {
    struct Log {
        std::size_t serial = 0;
        QString design;
        std::vector<RunStage> stages;
    };
    auto result = std::make_shared<Log>();
    engine_.query(
        [result](const ktSession &session) {
            result->serial = session.designSerial();
            result->design = QString::fromStdString(session.designPath());
            for (const ktSession::StageRecord &r : session.stageLog()) {
                result->stages.push_back({QString::fromStdString(r.stage),
                                          QString::fromStdString(r.detail), r.hpwl, r.seconds,
                                          r.stopped});
            }
        },
        this,
        [this, result] {
            if (result->serial == 0 ||
                !history_.update(result->serial, result->design, std::move(result->stages))) {
                return;
            }
            if (!history_.save()) {
                ktlog.warning("cannot write the run history {}", history_.file().toStdString());
            }
            runs_->refresh();
        });
}

void MainWindow::showProgress(const ProgressSample &sample,
                              const std::shared_ptr<const LayoutSnapshot> &layout) {
    metrics_->addSample(sample);
    if (sample.run != liveRun_) {
        // Shown once, when a run starts; after that the user's choice of tab stands.
        liveRun_ = sample.run;
        docks_[kMetricsDock]->raise();
    }
    if (layout) {
        layout_->setSnapshot(layout);
    }
    QString text =
        QStringLiteral("%1 %2 iteration %3/%4   HPWL %5")
            .arg(QString::fromStdString(sample.stage), QString::fromStdString(sample.phase))
            .arg(static_cast<qulonglong>(sample.iter + 1))
            .arg(static_cast<qulonglong>(sample.total))
            .arg(sample.hpwl, 0, 'e', 4);
    if (sample.overflow >= 0.0) {
        text += QStringLiteral("   overflow %1").arg(sample.overflow, 0, 'f', 4);
    }
    status_->setText(text);
}

void MainWindow::refreshLayout() {
    // Filled on the engine thread and read here once the query's completion
    // arrives, which is ordered after the write.
    auto result = std::make_shared<std::shared_ptr<const LayoutSnapshot>>();
    engine_.query(
        [result](const ktSession &session) {
            if (session.hasDesign()) {
                *result = LayoutSnapshot::capture(session);
            }
        },
        this,
        [this, result] {
            layout_->setSnapshot(*result);
            if (*result) {
                status_->setText(QStringLiteral("%1 cells   HPWL %2")
                                     .arg((*result)->cells.size())
                                     .arg((*result)->hpwl, 0, 'e', 6));
            }
            inspect(layout_->selection());  // the selected cell may have moved
        });
}

void MainWindow::inspect(std::optional<std::size_t> cell) {
    if (!cell) {
        properties_->clear();
        layout_->setNetOverlay({});
        return;
    }
    auto result = std::make_shared<std::optional<CellDetails>>();
    const std::size_t id = *cell;
    engine_.query(
        [result, id](const ktSession &session) {
            *result = CellDetails::lookup(session, id);
        },
        this,
        [this, result, id] {
            // A later click may have replaced this one while it was looked up.
            if (!*result || layout_->selection() != id) {
                return;
            }
            const CellDetails &d = **result;
            properties_->showCell(d);
            std::vector<LayoutView::NetOverlay> nets;
            const QPointF centre(d.x + d.width / 2.0, d.y + d.height / 2.0);
            for (const CellDetails::NetRef &n : d.nets) {
                if (!n.others.empty()) {
                    nets.push_back({centre, n.others});
                }
            }
            layout_->setNetOverlay(std::move(nets));
            docks_[kPropertiesDock]->raise();
        });
}

void MainWindow::setBusy(bool busy) {
    for (QAction *action : engineActions_) {
        action->setEnabled(!busy);
    }
    stop_->setEnabled(busy);
    status_->setText(busy ? QStringLiteral("Running...") : QStringLiteral("Ready"));
    if (busy) {
        QApplication::setOverrideCursor(Qt::BusyCursor);
        return;
    }
    QApplication::restoreOverrideCursor();
    refreshLayout();
    recordRun();
}

}  // namespace ktplace
