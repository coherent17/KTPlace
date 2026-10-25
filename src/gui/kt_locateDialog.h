#pragma once

#include "gui/kt_layout.h"

#include <QDialog>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace ktplace::gui {

class LocateDialog : public QDialog {
public:
    enum class Target { Cells, Nets };

    explicit LocateDialog(QWidget *parent = nullptr);
    ~LocateDialog() override;

    LocateDialog(const LocateDialog &) = delete;
    LocateDialog &operator=(const LocateDialog &) = delete;

    void setLayoutGeometry(std::shared_ptr<const LayoutGeometry> geometry);
    void search(const QString &pattern, Target target);
    void onChosen(
        std::function<void(Target target, std::vector<std::uint32_t> ids, bool zoom)> listener);
    [[nodiscard]] const std::vector<std::uint32_t> &isMatch() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
