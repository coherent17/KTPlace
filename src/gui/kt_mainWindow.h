#pragma once

#include "gui/kt_runner.h"

#include <QMainWindow>
#include <functional>
#include <memory>

namespace ktplace::gui {

class DetailsPanel;
class LayoutView;
class LocateDialog;
class LogView;
class TclConsole;
class VisibilityPanel;

class MainWindow : public QMainWindow {
public:
    explicit MainWindow(RunRequest request, QWidget *parent = nullptr);
    ~MainWindow() override;

    MainWindow(const MainWindow &) = delete;
    MainWindow &operator=(const MainWindow &) = delete;

    void openDesign(const QString &design);
    void run();
    void stop();

    [[nodiscard]] const RunRequest &getRequest() const;
    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] LayoutView *getLayoutView() const;
    [[nodiscard]] LogView *logView() const;
    [[nodiscard]] TclConsole *getTclConsole() const;
    [[nodiscard]] VisibilityPanel *getVisibilityPanel() const;
    [[nodiscard]] DetailsPanel *getDetailsPanel() const;
    [[nodiscard]] LocateDialog *locateDialog() const;
    [[nodiscard]] QString getCursorText() const;
    [[nodiscard]] QString getStatus() const;

    void onFinished(std::function<void(const RunResult &)> callback);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
