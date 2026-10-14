// @file kt_mainWindow.cc
// Implementation of MainWindow.

#include "gui/kt_mainWindow.h"

#include "kt_option.h"

#include "gui/kt_engine.h"
#include "gui/kt_layoutSnapshot.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"
#include "gui/kt_propertyPanel.h"
#include "gui/kt_tclConsole.h"
#include "session/kt_session.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <memory>
#include <utility>
#include <vector>

namespace ktplace {

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
    propertiesDock_ = new QDockWidget(QStringLiteral("Properties"), this);
    propertiesDock_->setObjectName(QStringLiteral("propertiesDock"));
    propertiesDock_->setWidget(properties_);
    addDockWidget(Qt::RightDockWidgetArea, propertiesDock_);
    resizeDocks({propertiesDock_}, {280}, Qt::Horizontal);

    log_ = new LogView(this);
    console_ = new TclConsole(engine_, this);
    console_->onBusyChanged = [this](bool busy) {
        setBusy(busy);
    };

    logDock_ = new QDockWidget(QStringLiteral("Log"), this);
    logDock_->setObjectName(QStringLiteral("logDock"));
    logDock_->setWidget(log_);
    consoleDock_ = new QDockWidget(QStringLiteral("Tcl Console"), this);
    consoleDock_->setObjectName(QStringLiteral("consoleDock"));
    consoleDock_->setWidget(console_);
    addDockWidget(Qt::BottomDockWidgetArea, logDock_);
    addDockWidget(Qt::BottomDockWidgetArea, consoleDock_);
    tabifyDockWidget(logDock_, consoleDock_);
    consoleDock_->raise();
    resizeDocks({consoleDock_}, {300}, Qt::Vertical);

    status_ = new QLabel(QStringLiteral("Ready"), this);
    statusBar()->addWidget(status_, 1);
    cursor_ = new QLabel(this);
    cursor_->setMinimumWidth(200);
    statusBar()->addPermanentWidget(cursor_);

    buildMenus();
    console_->setFocus();
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
    if (!shortcut.isEmpty()) {
        action->setShortcut(shortcut);
    }
    connect(action, &QAction::triggered, this, [this, script] {
        console_->run(script);
    });
    engineActions_ << action;
    return action;
}

void MainWindow::buildMenus() {
    QMenu *file = menuBar()->addMenu(QStringLiteral("&File"));
    QAction *open = file->addAction(QStringLiteral("&Open Design..."));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Open design"));
        if (!dir.isEmpty()) {
            console_->run("read_design " + tclQuote(dir));
        }
    });
    engineActions_ << open;
    QAction *source = file->addAction(QStringLiteral("&Source Script..."));
    connect(source, &QAction::triggered, this, [this] {
        const QString path =
            QFileDialog::getOpenFileName(this, QStringLiteral("Source Tcl script"), QString(),
                                         QStringLiteral("Tcl scripts (*.tcl);;All files (*)"));
        if (!path.isEmpty()) {
            console_->run("source " + tclQuote(path));
        }
    });
    engineActions_ << source;
    QAction *write = file->addAction(QStringLiteral("&Write Placement..."));
    write->setShortcut(QKeySequence::Save);
    connect(write, &QAction::triggered, this, [this] {
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Write placement"), QStringLiteral("placed.pl"),
            QStringLiteral("Bookshelf placement (*.pl);;All files (*)"));
        if (!path.isEmpty()) {
            console_->run("write_design " + tclQuote(path));
        }
    });
    engineActions_ << write;
    file->addSeparator();
    QAction *quit = file->addAction(QStringLiteral("&Quit"));
    quit->setShortcut(QKeySequence::Quit);
    connect(quit, &QAction::triggered, this, &QWidget::close);

    QMenu *place = menuBar()->addMenu(QStringLiteral("&Place"));
    engineAction(place, QStringLiteral("&Global Place"), QStringLiteral("global_place"),
                 QKeySequence(Qt::Key_F5));
    engineAction(place, QStringLiteral("&Legalize"), QStringLiteral("legalize"),
                 QKeySequence(Qt::Key_F6));
    engineAction(place, QStringLiteral("&Detail Place"), QStringLiteral("detail_place"),
                 QKeySequence(Qt::Key_F7));
    place->addSeparator();
    // The fixed flow's stages, in one script: scripts/flow.tcl without the I/O.
    engineAction(place, QStringLiteral("Run &Full Flow"),
                 QStringLiteral("global_place; legalize; check_placement {after legalization}; "
                                "detail_place; check_placement {after detail placement}; "
                                "report_hpwl"),
                 QKeySequence(Qt::Key_F9));

    QMenu *report = menuBar()->addMenu(QStringLiteral("&Report"));
    engineAction(report, QStringLiteral("&Design"), QStringLiteral("report_design"));
    engineAction(report, QStringLiteral("&HPWL"), QStringLiteral("report_hpwl"));
    engineAction(report, QStringLiteral("&Check Placement"), QStringLiteral("check_placement"));
    engineAction(report, QStringLiteral("&Timing"), QStringLiteral("report_timing"));
    engineAction(report, QStringLiteral("&Parameters"), QStringLiteral("report_params"));

    QMenu *view = menuBar()->addMenu(QStringLiteral("&View"));
    QAction *fit = view->addAction(QStringLiteral("Zoom &Fit"));
    fit->setShortcut(QKeySequence(Qt::Key_F));
    fit->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    layout_->addAction(fit);
    connect(fit, &QAction::triggered, this, [this] {
        layout_->zoomFit();
    });
    view->addSeparator();
    view->addAction(propertiesDock_->toggleViewAction());
    view->addAction(logDock_->toggleViewAction());
    view->addAction(consoleDock_->toggleViewAction());
    QAction *trace = view->addAction(QStringLiteral("Show &Trace in Log"));
    trace->setCheckable(true);
    connect(trace, &QAction::toggled, this, [this](bool on) {
        log_->setShowTrace(on);
    });

    QMenu *help = menuBar()->addMenu(QStringLiteral("&Help"));
    engineAction(help, QStringLiteral("&Commands"), QStringLiteral("help"));
    QAction *about = help->addAction(QStringLiteral("&About KTPlace"));
    connect(about, &QAction::triggered, this, [this] {
        QMessageBox::about(
            this, QStringLiteral("About KTPlace"),
            QStringLiteral("KTPlace v%1\nKnow Thyself Placement Engine")
                .arg(QString::fromUtf8(VERSION.data(), static_cast<qsizetype>(VERSION.size()))));
    });
}

void MainWindow::refreshLayout() {
    // The shared_ptr is filled on the engine thread and read on this one once
    // the query's completion arrives, which is ordered after the write.
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
            // The selected cell may have moved: look it up again.
            inspect(layout_->selection());
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
            propertiesDock_->raise();
        });
}

void MainWindow::setBusy(bool busy) {
    for (QAction *action : engineActions_) {
        action->setEnabled(!busy);
    }
    if (busy) {
        status_->setText(QStringLiteral("Running..."));
        QApplication::setOverrideCursor(Qt::BusyCursor);
    } else {
        status_->setText(QStringLiteral("Ready"));
        QApplication::restoreOverrideCursor();
        refreshLayout();
    }
}

}  // namespace ktplace
