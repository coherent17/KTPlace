#pragma once

#include "gui/kt_layout.h"

#include <QPointF>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>

namespace ktplace::gui {

class LayoutView : public QWidget {
public:
    explicit LayoutView(QWidget *parent = nullptr);
    ~LayoutView() override;

    LayoutView(const LayoutView &) = delete;
    LayoutView &operator=(const LayoutView &) = delete;

    void setFrame(LayoutFrame frame);
    [[nodiscard]] const LayoutFrame &frame() const;

    void zoomFit();
    void selectAt(QPointF screen);
    [[nodiscard]] std::optional<std::uint32_t> selectedCell() const;
    void zoomBy(double factor, QPointF anchor);

    [[nodiscard]] QPointF worldToScreen(QPointF world) const;
    [[nodiscard]] QPointF screenToWorld(QPointF screen) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
