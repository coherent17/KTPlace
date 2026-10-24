#pragma once

#include "gui/kt_displayStyle.h"
#include "gui/kt_layout.h"
#include "gui/kt_selectionSet.h"

#include <QImage>
#include <QPointF>
#include <QWidget>
#include <cstddef>
#include <cstdint>
#include <functional>
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

    [[nodiscard]] SelectionSet &selection();
    [[nodiscard]] const SelectionSet &selection() const;
    void selectAt(QPointF screen, bool additive = false);
    [[nodiscard]] std::optional<std::uint32_t> selectedCell() const;
    std::size_t showConnectivity();
    void hideConnectivity();

    [[nodiscard]] const DisplayStyle &style() const;
    void setStyle(const DisplayStyle &newStyle);
    void onStyleChanged(std::function<void(const DisplayStyle &)> listener);
    void showCongestion(bool on, std::size_t binsAcross = 256);
    [[nodiscard]] bool showingCongestion() const;

    void zoomFit();
    void zoomTo(const Bounds &area);
    bool zoomToSelection();
    void zoomBy(double factor, QPointF anchor);
    void onCursorMoved(std::function<void(std::optional<QPointF>)> listener);
    [[nodiscard]] QImage capture();

    [[nodiscard]] QPointF worldToScreen(QPointF world) const;
    [[nodiscard]] QPointF screenToWorld(QPointF screen) const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::gui
