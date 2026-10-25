#pragma once

#include <QString>
#include <QWidget>
#include <functional>
#include <memory>

namespace ktplace::gui {

class TclConsole : public QWidget {
public:
    enum class Kind { Command, Result, Error };

    explicit TclConsole(QWidget *parent = nullptr);
    ~TclConsole() override;

    TclConsole(const TclConsole &) = delete;
    TclConsole &operator=(const TclConsole &) = delete;

    void onCommand(std::function<void(const QString &)> handler);
    void append(const QString &text, Kind kind);
    void setBusy(bool busy);
    void focusInput();
    [[nodiscard]] QString getTranscript() const;
    void submit(const QString &command);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
