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

    [[nodiscard]] const RunRequest &request() const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] LayoutView *layoutView() const;
    [[nodiscard]] LogView *logView() const;
    [[nodiscard]] TclConsole *tclConsole() const;
    [[nodiscard]] VisibilityPanel *visibilityPanel() const;
    [[nodiscard]] DetailsPanel *detailsPanel() const;
    [[nodiscard]] LocateDialog *locateDialog() const;
    [[nodiscard]] QString cursorText() const;
    [[nodiscard]] QString status() const;

    void onFinished(std::function<void(const RunResult &)> callback);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
