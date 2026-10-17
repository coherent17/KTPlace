#include "gui/kt_mainWindow.h"

#include "gui/kt_layoutView.h"
#include "gui/kt_logView.h"

#include <QAction>
#include <QCloseEvent>
#include <QDockWidget>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QMenuBar>
#include <QStatusBar>
#include <QToolBar>
#include <filesystem>
#include <utility>

namespace ktplace::gui {

namespace {

QString describe(const LayoutFrame &frame) {
    return QStringLiteral("%1   HPWL %2")
        .arg(QString::fromStdString(frame.stage))
        .arg(frame.hpwl, 0, 'e', 6);
}

std::string defaultWorkDir(const QString &design) {
    return (std::filesystem::current_path() / "output" / QFileInfo(design).fileName().toStdString())
        .string();
}

}  // namespace

class MainWindow::Impl {
public:
    Impl(MainWindow &window, RunRequest request)
        : window(window),
          keepWorkDir(request.design.empty() && !request.workDir.empty()),
          request(std::move(request)),
          view(new LayoutView(&window)),
          log(new LogView(&window)),
          stage(new QLabel(&window)),
          runner(&window, {.frame =
                               [this](LayoutFrame frame) {
                                   stage->setText(describe(frame));
                                   view->setFrame(std::move(frame));
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
    QAction *openAction = nullptr;
    QAction *scriptAction = nullptr;
    QAction *runAction = nullptr;
    QAction *stopAction = nullptr;
    std::function<void(const RunResult &)> finishedCallback;
    Runner runner;

    void build();
    void refresh();
    void finished(const RunResult &result);
};

void MainWindow::Impl::build() {
    window.setWindowTitle(QStringLiteral("KTPlace"));
    window.setCentralWidget(view);

    auto *dock = new QDockWidget(QStringLiteral("Log"), &window);
    dock->setObjectName(QStringLiteral("log"));
    dock->setWidget(log);
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    window.addDockWidget(Qt::BottomDockWidgetArea, dock);

    openAction = new QAction(QStringLiteral("Open Design..."), &window);
    openAction->setShortcut(QKeySequence::Open);
    QObject::connect(openAction, &QAction::triggered, &window, [this] {
        const QString design =
            QFileDialog::getExistingDirectory(&window, QStringLiteral("Open a design directory"),
                                              QString::fromStdString(request.design));
        if (!design.isEmpty()) {
            window.openDesign(design);
        }
    });

    scriptAction = new QAction(QStringLiteral("Tcl Script..."), &window);
    QObject::connect(scriptAction, &QAction::triggered, &window, [this] {
        request.script = QFileDialog::getOpenFileName(&window, QStringLiteral("Configure the run"),
                                                      QString::fromStdString(request.script),
                                                      QStringLiteral("Tcl scripts (*.tcl)"))
                             .toStdString();
        refresh();
    });

    runAction = new QAction(QStringLiteral("Run"), &window);
    runAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    QObject::connect(runAction, &QAction::triggered, &window, [this] {
        window.run();
    });

    stopAction = new QAction(QStringLiteral("Stop"), &window);
    stopAction->setShortcut(QKeySequence(Qt::Key_Escape));
    QObject::connect(stopAction, &QAction::triggered, &window, [this] {
        window.stop();
    });

    auto *fitAction = new QAction(QStringLiteral("Zoom Fit"), &window);
    fitAction->setShortcut(QKeySequence(Qt::Key_F));
    QObject::connect(fitAction, &QAction::triggered, view, &LayoutView::zoomFit);

    QMenu *file = window.menuBar()->addMenu(QStringLiteral("&File"));
    file->addAction(openAction);
    file->addAction(scriptAction);
    QMenu *place = window.menuBar()->addMenu(QStringLiteral("&Place"));
    place->addAction(runAction);
    place->addAction(stopAction);
    QMenu *show = window.menuBar()->addMenu(QStringLiteral("&View"));
    show->addAction(fitAction);
    show->addAction(dock->toggleViewAction());

    QToolBar *tools = window.addToolBar(QStringLiteral("Run"));
    tools->setObjectName(QStringLiteral("run"));
    tools->addAction(openAction);
    tools->addAction(scriptAction);
    tools->addSeparator();
    tools->addAction(runAction);
    tools->addAction(stopAction);

    window.statusBar()->addPermanentWidget(stage);
    window.resize(1280, 860);
    refresh();
}

void MainWindow::Impl::refresh() {
    const bool busy = runner.running();
    openAction->setEnabled(!busy);
    scriptAction->setEnabled(!busy);
    runAction->setEnabled(!busy && !request.design.empty());
    stopAction->setEnabled(busy);
    if (busy) {
        window.statusBar()->showMessage(
            QStringLiteral("Placing %1 ...").arg(QString::fromStdString(request.design)));
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
    refresh();
    const QString text = QString::fromStdString(result.message);
    switch (result.outcome) {
        case RunResult::Outcome::Placed:
            window.statusBar()->showMessage(QStringLiteral("Placed: %1").arg(text));
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
    pImpl->view->setFrame({});
    pImpl->stage->clear();
    pImpl->refresh();
}

void MainWindow::run() {
    if (pImpl->request.design.empty()) {
        return;
    }
    if (pImpl->request.workDir.empty()) {
        pImpl->request.workDir = defaultWorkDir(QString::fromStdString(pImpl->request.design));
    }
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

QString MainWindow::status() const {
    return statusBar()->currentMessage();
}

void MainWindow::onFinished(std::function<void(const RunResult &)> callback) {
    pImpl->finishedCallback = std::move(callback);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    pImpl->runner.stop();
    event->accept();
}

}  // namespace ktplace::gui
