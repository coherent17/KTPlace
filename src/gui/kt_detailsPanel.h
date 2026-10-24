#pragma once

#include "gui/kt_layout.h"
#include "gui/kt_selectionSet.h"

#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>

namespace ktplace::gui {

class DetailsPanel : public QWidget {
public:
    explicit DetailsPanel(QWidget *parent = nullptr);
    ~DetailsPanel() override;

    DetailsPanel(const DetailsPanel &) = delete;
    DetailsPanel &operator=(const DetailsPanel &) = delete;

    void describe(const LayoutFrame &frame, const SelectionSet &selection);
    void onNetChosen(std::function<void(std::uint32_t net, bool zoom)> listener);
    [[nodiscard]] QString title() const;
    [[nodiscard]] int netRows() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
