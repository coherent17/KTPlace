#pragma once

#include "gui/kt_displayStyle.h"

#include <QWidget>
#include <functional>
#include <memory>

namespace ktplace::gui {

class VisibilityPanel : public QWidget {
public:
    explicit VisibilityPanel(QWidget *parent = nullptr);
    ~VisibilityPanel() override;

    VisibilityPanel(const VisibilityPanel &) = delete;
    VisibilityPanel &operator=(const VisibilityPanel &) = delete;

    void setStyle(const DisplayStyle &style);
    [[nodiscard]] const DisplayStyle &getStyle() const;
    void onChanged(std::function<void(const DisplayStyle &)> listener);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
