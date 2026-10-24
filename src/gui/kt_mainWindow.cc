#include "gui/kt_mainWindow.h"

#include "kt_option.h"

#include "gui/kt_detailsPanel.h"
#include "gui/kt_dialogPlacement.h"
#include "gui/kt_layoutCommands.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_locateDialog.h"
#include "gui/kt_logView.h"
#include "gui/kt_tclConsole.h"
#include "gui/kt_visibilityPanel.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QToolBar>
#include <filesystem>
#include <optional>
#include <utility>

namespace ktplace::gui {

namespace {

constexpr int liveDetailsMs = 250;
constexpr int layoutVersion = 2;
constexpr qsizetype recentLimit = 8;
const QString settingsOrganisation = QStringLiteral("KTPlace");
const QString settingsApplication = QStringLiteral("KTPlace");
const QString geometryKey = QStringLiteral("window/geometry");
const QString stateKey = QStringLiteral("window/state");
const QString recentKey = QStringLiteral("recentDesigns");

QString describe(const LayoutFrame &frame) {
    return QStringLiteral("%1   HPWL %2")
        .arg(QString::fromStdString(frame.stage))
        .arg(frame.hpwl, 0, 'e', 6);
}

std::string defaultWorkDir(const QString &design) {
    return (std::filesystem::current_path() / "output" / QFileInfo(design).fileName().toStdString())
        .string();
}

QDockWidget *dockFor(QMainWindow &window, const QString &title, const QString &name,
                     QWidget *content) {
    auto *dock = new QDockWidget(title, &window);
    dock->setObjectName(name);
    dock->setWidget(content);
    return dock;
}

QString chooseExisting(QWidget &parent, const QString &title, const QString &start,
                       QFileDialog::FileMode mode, const QString &filter = {}) {
    QFileDialog dialog(&parent, title, start, filter);
    dialog.setFileMode(mode);
    placeOver(dialog, parent);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0) : QString();
}

QString chooseNewFile(QWidget &parent, const QString &title, const QString &start,
                      const QString &filter) {
    QFileDialog dialog(&parent, title, start, filter);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix(QStringLiteral("png"));
    placeOver(dialog, parent);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0) : QString();
}

QString versionText() {
    return QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size()));
}

}  // namespace

class MainWindow::Impl {
public:
    Impl(MainWindow &owner, RunRequest initialRequest)
        : window(owner),
          keepWorkDir(initialRequest.design.empty() && !initialRequest.workDir.empty()),
          request(std::move(initialRequest)),
          view(new LayoutView(&owner)),
          log(new LogView(&owner)),
          stage(new QLabel(&owner)),
          cursor(new QLabel(&owner)),
          console(new TclConsole(&owner)),
          visibility(new VisibilityPanel(&owner)),
          details(new DetailsPanel(&owner)),
          locate(new LocateDialog(&owner)),
          runner(&owner, {.frame =
                              [this](LayoutFrame frame) {
                                  showFrame(std::move(frame));
                              },
                          .finished =
                              [this](const RunResult &result) {
                                  finished(result);
                              }}) {}

    MainWindow &window;
    bool keepWorkDir;
    RunRequest request;
    LayoutView *view;
    LogView *log;
    QLabel *stage;
    QLabel *cursor;
    TclConsole *console;
    VisibilityPanel *visibility;
    DetailsPanel *details;
    LocateDialog *locate;
    QDockWidget *logDock = nullptr;
    QDockWidget *detailsDock = nullptr;
    QDockWidget *visibilityDock = nullptr;
    QDockWidget *consoleDock = nullptr;
    QMenu *recentMenu = nullptr;
    QAction *openAction = nullptr;
    QAction *scriptAction = nullptr;
    QAction *runAction = nullptr;
    QAction *stopAction = nullptr;
    QTimer detailsTimer;
    bool loading = false;
    std::function<void(const RunResult &)> finishedCallback;
    Runner runner;
    std::optional<LayoutCommandBinding> commands;

    void build();
    void buildPanels();
    void buildActions();
    void restoreLayout();
    void refresh();
    void showFrame(LayoutFrame frame);
    void rememberDesign(const QString &design) const;
    void fillRecentMenu();
    void saveViewImage();
    [[nodiscard]] RunRequest consoleRequest() const;
    void finished(const RunResult &result);

    [[nodiscard]] QAction *action(const QString &text, const QKeySequence &shortcut,
                                  std::function<void()> trigger) const {
        auto *created = new QAction(text, &window);
        created->setShortcut(shortcut);
        QObject::connect(created, &QAction::triggered, &window, std::move(trigger));
        return created;
    }
};

void MainWindow::Impl::build() {
    window.setWindowTitle(QStringLiteral("KTPlace"));
    window.setCentralWidget(view);
    buildPanels();
    buildActions();
    window.statusBar()->addPermanentWidget(cursor);
    window.statusBar()->addPermanentWidget(stage);
    cursor->setMinimumWidth(
        cursor->fontMetrics().horizontalAdvance(QStringLiteral("x 000000000.0   y 000000000.0")));
    restoreLayout();
    commands.emplace(*view);
    refresh();
}

void MainWindow::Impl::buildPanels() {
    logDock = dockFor(window, QStringLiteral("Log"), QStringLiteral("log"), log);
    logDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    window.addDockWidget(Qt::BottomDockWidgetArea, logDock);
    consoleDock =
        dockFor(window, QStringLiteral("Tcl Console"), QStringLiteral("tclConsole"), console);
    consoleDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    window.setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::North);
    window.tabifyDockWidget(logDock, consoleDock);
    logDock->setTitleBarWidget(new QWidget(logDock));
    consoleDock->setTitleBarWidget(new QWidget(consoleDock));
    logDock->raise();

    detailsDock = dockFor(window, QStringLiteral("Details"), QStringLiteral("details"), details);
    window.addDockWidget(Qt::LeftDockWidgetArea, detailsDock);
    visibilityDock =
        dockFor(window, QStringLiteral("Visibility"), QStringLiteral("visibility"), visibility);
    window.addDockWidget(Qt::RightDockWidgetArea, visibilityDock);
    visibilityDock->hide();

    console->onCommand([this](const QString &command) {
        if (!runner.execute(command.toStdString(), consoleRequest())) {
            console->append(QStringLiteral("busy: wait for the current run, or press Stop"),
                            TclConsole::Kind::Error);
            return;
        }
        refresh();
    });

    visibility->setStyle(view->style());
    visibility->onChanged([this](const DisplayStyle &style) {
        view->setStyle(style);
    });
    view->onStyleChanged([this](const DisplayStyle &style) {
        visibility->setStyle(style);
    });

    detailsTimer.setSingleShot(true);
    QObject::connect(&detailsTimer, &QTimer::timeout, &window, [this] {
        details->describe(view->frame(), view->selection());
    });
    view->selection().subscribe([this] {
        detailsTimer.start(0);
    });
    details->onNetChosen([this](std::uint32_t net, bool zoom) {
        view->selection().markNets({net});
        if (zoom) {
            if (const std::optional<Bounds> area = view->frame().netBounds(net)) {
                view->zoomTo(*area);
            }
        }
    });

    view->onCursorMoved([this](std::optional<QPointF> at) {
        cursor->setText(
            at ? QStringLiteral("x %1   y %2").arg(at->x(), 0, 'f', 1).arg(at->y(), 0, 'f', 1)
               : QString());
    });

    locate->onChosen(
        [this](LocateDialog::Target target, std::vector<std::uint32_t> ids, bool zoom) {
            if (target == LocateDialog::Target::Cells) {
                view->selection().selectCells(std::move(ids));
            } else {
                view->selection().markNets(std::move(ids));
            }
            if (zoom) {
                view->zoomToSelection();
            }
        });
}

void MainWindow::Impl::buildActions() {
    openAction = action(QStringLiteral("Open Design..."), QKeySequence::Open, [this] {
        const QString design =
            chooseExisting(window, QStringLiteral("Open a design directory"),
                           QString::fromStdString(request.design), QFileDialog::Directory);
        if (!design.isEmpty()) {
            window.openDesign(design);
        }
    });
    scriptAction = action(QStringLiteral("Tcl Script..."), {}, [this] {
        const QString script = chooseExisting(
            window, QStringLiteral("Configure the run"), QString::fromStdString(request.script),
            QFileDialog::ExistingFile, QStringLiteral("Tcl scripts (*.tcl)"));
        if (!script.isEmpty()) {
            request.script = script.toStdString();
        }
        refresh();
    });
    runAction = action(QStringLiteral("Run"), QKeySequence(Qt::CTRL | Qt::Key_R), [this] {
        window.run();
    });
    stopAction = action(QStringLiteral("Stop"), QKeySequence(Qt::Key_Escape), [this] {
        window.stop();
    });
    QAction *saveImage = action(QStringLiteral("Save View Image..."),
                                QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_S), [this] {
                                    saveViewImage();
                                });
    QAction *quit = action(QStringLiteral("Quit"), QKeySequence::Quit, [this] {
        window.close();
    });
    QAction *locateAction = action(QStringLiteral("Locate..."), QKeySequence::Find, [this] {
        locate->setLayoutGeometry(view->frame().geometry);
        if (!locate->isVisible()) {
            placeOver(*locate, window);
        }
        locate->show();
        locate->raise();
        locate->activateWindow();
    });
    QAction *selectNone = action(QStringLiteral("Select None"),
                                 QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A), [this] {
                                     view->selection().clear();
                                 });
    QAction *fit = action(QStringLiteral("Zoom Fit"), QKeySequence(Qt::Key_F), [this] {
        view->zoomFit();
    });
    QAction *toSelection =
        action(QStringLiteral("Zoom to Selection"), QKeySequence(Qt::Key_Z), [this] {
            view->zoomToSelection();
        });
    QAction *showLog = action(QStringLiteral("Log"), QKeySequence(Qt::CTRL | Qt::Key_L), [this] {
        logDock->show();
        logDock->raise();
    });
    QAction *showConsole =
        action(QStringLiteral("Tcl Console"), QKeySequence(Qt::CTRL | Qt::Key_T), [this] {
            consoleDock->show();
            consoleDock->raise();
            console->focusInput();
        });
    QAction *keys = action(QStringLiteral("Mouse and Keys"), {}, [this] {
        showNote(window, QStringLiteral("Mouse and Keys"),
                 QStringLiteral(
                     "<table cellpadding=3>"
                     "<tr><td><b>Click</b></td><td>select a cell</td></tr>"
                     "<tr><td><b>Ctrl+click</b></td><td>add or remove a cell</td></tr>"
                     "<tr><td><b>Drag</b> or <b>middle drag</b></td><td>pan</td></tr>"
                     "<tr><td><b>Shift+drag</b></td><td>zoom to the box</td></tr>"
                     "<tr><td><b>Wheel</b>, <b>+</b>, <b>-</b></td><td>zoom</td></tr>"
                     "<tr><td><b>F</b></td><td>zoom to fit</td></tr>"
                     "<tr><td><b>Z</b></td><td>zoom to the selection</td></tr>"
                     "<tr><td><b>Ctrl+F</b></td><td>locate cells or nets</td></tr>"
                     "<tr><td><b>Ctrl+Shift+A</b></td><td>select none</td></tr>"
                     "<tr><td><b>Right-click</b></td><td>routing demand and view options</td></tr>"
                     "</table>"));
    });
    QAction *about = action(QStringLiteral("About KTPlace"), {}, [this] {
        showNote(window, QStringLiteral("About KTPlace"),
                 QStringLiteral("<h3>KTPlace %1</h3><p>Know Thyself Placement "
                                "Engine: global placement, legalization and "
                                "detailed placement for standard-cell designs.</p>")
                     .arg(versionText()));
    });

    QMenu *file = window.menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(openAction);
    recentMenu = file->addMenu(QStringLiteral("Recent Designs"));
    QObject::connect(recentMenu, &QMenu::aboutToShow, &window, [this] {
        fillRecentMenu();
    });
    file->addAction(scriptAction);
    file->addSeparator();
    file->addAction(saveImage);
    file->addSeparator();
    file->addAction(quit);

    QMenu *edit = window.menuBar()->addMenu(QStringLiteral("&Edit"));
    edit->addAction(locateAction);
    edit->addAction(selectNone);

    QMenu *show = window.menuBar()->addMenu(QStringLiteral("&View"));
    show->addAction(fit);
    show->addAction(toSelection);
    show->addSeparator();
    for (QDockWidget *dock : window.findChildren<QDockWidget *>()) {
        if (dock != logDock && dock != consoleDock) {
            show->addAction(dock->toggleViewAction());
        }
    }
    show->addAction(showLog);
    show->addAction(showConsole);

    QMenu *place = window.menuBar()->addMenu(QStringLiteral("&Place"));
    place->addAction(runAction);
    place->addAction(stopAction);

    QMenu *help = window.menuBar()->addMenu(QStringLiteral("&Help"));
    help->addAction(keys);
    help->addAction(about);

    QToolBar *runTools = window.addToolBar(QStringLiteral("Run"));
    runTools->setObjectName(QStringLiteral("run"));
    runTools->addAction(openAction);
    runTools->addAction(scriptAction);
    runTools->addSeparator();
    runTools->addAction(runAction);
    runTools->addAction(stopAction);
    QToolBar *viewTools = window.addToolBar(QStringLiteral("View"));
    viewTools->setObjectName(QStringLiteral("view"));
    viewTools->addAction(fit);
    viewTools->addAction(toSelection);
    viewTools->addAction(locateAction);
    viewTools->addSeparator();
    QAction *showVisibility = visibilityDock->toggleViewAction();
    showVisibility->setToolTip(QStringLiteral("Show or hide the Visibility panel"));
    viewTools->addAction(showVisibility);
}

void MainWindow::Impl::restoreLayout() {
    const QSettings settings(settingsOrganisation, settingsApplication);
    if (!settings.contains(geometryKey)) {
        window.resize(1280, 860);
        return;
    }
    window.restoreGeometry(settings.value(geometryKey).toByteArray());
    window.restoreState(settings.value(stateKey).toByteArray(), layoutVersion);
}

void MainWindow::Impl::rememberDesign(const QString &design) const {
    QSettings settings(settingsOrganisation, settingsApplication);
    QStringList recent = settings.value(recentKey).toStringList();
    recent.removeAll(design);
    recent.prepend(design);
    settings.setValue(recentKey, recent.mid(0, recentLimit));
}

void MainWindow::Impl::fillRecentMenu() {
    recentMenu->clear();
    const QSettings settings(settingsOrganisation, settingsApplication);
    const QStringList recent = settings.value(recentKey).toStringList();
    for (const QString &design : recent) {
        QAction *entry = recentMenu->addAction(design, &window, [this, design] {
            window.openDesign(design);
        });
        entry->setEnabled(!runner.running() && QFileInfo(design).isDir());
    }
    if (recent.isEmpty()) {
        recentMenu->addAction(QStringLiteral("No recent designs"))->setEnabled(false);
        return;
    }
    recentMenu->addSeparator();
    recentMenu->addAction(QStringLiteral("Clear Recent Designs"), &window, [] {
        QSettings(settingsOrganisation, settingsApplication).remove(recentKey);
    });
}

void MainWindow::Impl::saveViewImage() {
    const QString file =
        chooseNewFile(window, QStringLiteral("Save the view as an image"),
                      QStringLiteral("layout.png"), QStringLiteral("Images (*.png *.jpg)"));
    if (file.isEmpty()) {
        return;
    }
    if (view->capture().save(file)) {
        window.statusBar()->showMessage(QStringLiteral("Saved %1").arg(file));
    } else {
        showNote(window, QStringLiteral("Save View Image"),
                 QStringLiteral("Cannot write %1").arg(file.toHtmlEscaped()));
    }
}

void MainWindow::Impl::showFrame(LayoutFrame frame) {
    stage->setText(describe(frame));
    const bool newGeometry = frame.geometry != view->frame().geometry;
    view->setFrame(std::move(frame));
    if (newGeometry) {
        locate->setLayoutGeometry(view->frame().geometry);
        detailsTimer.start(0);
    } else if (!detailsTimer.isActive() && !view->selection().empty()) {
        detailsTimer.start(liveDetailsMs);
    }
}

RunRequest MainWindow::Impl::consoleRequest() const {
    RunRequest session = request;
    if (session.workDir.empty()) {
        session.workDir = session.design.empty()
                              ? (std::filesystem::current_path() / "output" / "session").string()
                              : defaultWorkDir(QString::fromStdString(session.design));
    }
    return session;
}

void MainWindow::Impl::refresh() {
    const bool busy = runner.running();
    console->setBusy(busy);
    openAction->setEnabled(!busy);
    scriptAction->setEnabled(!busy);
    runAction->setEnabled(!busy && !request.design.empty());
    stopAction->setEnabled(busy);
    if (busy) {
        window.statusBar()->showMessage(
            QStringLiteral("%1 %2 ...")
                .arg(loading ? QStringLiteral("Loading") : QStringLiteral("Placing"),
                     QString::fromStdString(request.design)));
        return;
    }
    QString message = request.design.empty() ? QStringLiteral("Open a design to place")
                                             : QString::fromStdString(request.design);
    if (!request.script.empty()) {
        message += QStringLiteral("   script %1").arg(QString::fromStdString(request.script));
    }
    window.statusBar()->showMessage(message);
}

void MainWindow::Impl::finished(const RunResult &result) {
    loading = false;
    refresh();
    if (result.fromConsole) {
        if (!result.message.empty()) {
            console->append(QString::fromStdString(result.message),
                            result.outcome == RunResult::Outcome::Evaluated
                                ? TclConsole::Kind::Result
                                : TclConsole::Kind::Error);
        }
        if (finishedCallback) {
            finishedCallback(result);
        }
        return;
    }
    const QString text = QString::fromStdString(result.message);
    switch (result.outcome) {
        case RunResult::Outcome::Placed:
            window.statusBar()->showMessage(QStringLiteral("Placed: %1").arg(text));
            break;
        case RunResult::Outcome::Loaded:
            window.statusBar()->showMessage(
                QStringLiteral("Loaded %1: Run places it, or step through it in the Tcl Console")
                    .arg(text));
            break;
        case RunResult::Outcome::Evaluated:
            break;
        case RunResult::Outcome::Stopped:
            window.statusBar()->showMessage(QStringLiteral("Stopped"));
            break;
        case RunResult::Outcome::Failed:
            window.statusBar()->showMessage(QStringLiteral("Failed: %1").arg(text));
            break;
    }
    if (finishedCallback) {
        finishedCallback(result);
    }
}

MainWindow::MainWindow(RunRequest request, QWidget *parent)
    : QMainWindow(parent), pImpl(std::make_unique<Impl>(*this, std::move(request))) {
    pImpl->build();
}

MainWindow::~MainWindow() = default;

void MainWindow::openDesign(const QString &design) {
    if (pImpl->runner.running()) {
        return;
    }
    pImpl->request.design = design.toStdString();
    if (!pImpl->keepWorkDir) {
        pImpl->request.workDir = defaultWorkDir(design);
    }
    pImpl->rememberDesign(design);
    pImpl->view->setFrame({});
    pImpl->stage->clear();
    pImpl->log->clear();
    pImpl->loading = pImpl->runner.open(pImpl->consoleRequest());
    pImpl->refresh();
}

void MainWindow::run() {
    if (pImpl->request.design.empty()) {
        return;
    }
    if (pImpl->request.workDir.empty()) {
        pImpl->request.workDir = defaultWorkDir(QString::fromStdString(pImpl->request.design));
    }
    pImpl->rememberDesign(QString::fromStdString(pImpl->request.design));
    pImpl->log->clear();
    if (pImpl->runner.start(pImpl->request)) {
        pImpl->refresh();
    }
}

void MainWindow::stop() {
    pImpl->runner.stop();
}

const RunRequest &MainWindow::request() const {
    return pImpl->request;
}

bool MainWindow::running() const {
    return pImpl->runner.running();
}

LayoutView *MainWindow::layoutView() const {
    return pImpl->view;
}

LogView *MainWindow::logView() const {
    return pImpl->log;
}

TclConsole *MainWindow::tclConsole() const {
    return pImpl->console;
}

VisibilityPanel *MainWindow::visibilityPanel() const {
    return pImpl->visibility;
}

DetailsPanel *MainWindow::detailsPanel() const {
    return pImpl->details;
}

LocateDialog *MainWindow::locateDialog() const {
    return pImpl->locate;
}

QString MainWindow::cursorText() const {
    return pImpl->cursor->text();
}

QString MainWindow::status() const {
    return statusBar()->currentMessage();
}

void MainWindow::onFinished(std::function<void(const RunResult &)> callback) {
    pImpl->finishedCallback = std::move(callback);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    pImpl->runner.stop();
    QSettings settings(settingsOrganisation, settingsApplication);
    settings.setValue(geometryKey, saveGeometry());
    settings.setValue(stateKey, saveState(layoutVersion));
    event->accept();
}

}  // namespace ktplace::gui
