// @file kt_layoutView.h
// The layout view: the die, its rows and every cell, with zoom, pan and pick.
//
// Draws a LayoutSnapshot (see kt_layoutSnapshot.h), never the live design.
//
// Drawing: the picture is rendered into a cached image only when the view or
// the snapshot changes; a repaint for any other reason (an overlay, a window
// uncovered) is one image copy. A cell smaller than a pixel is not drawn as a
// rectangle: its area is added to the pixel it falls in, and those sums are
// shown as a density map. So a zoomed-out million-cell design is one pass over
// the cells, and zooming in turns the map into rectangles cell by cell, with
// no mode switch and no cell left out.
//
// Interaction: wheel to zoom about the cursor, drag to pan, click to select,
// F (or View > Zoom Fit) to fit the die, + and - to zoom about the centre.

#pragma once

#include "gui/kt_layoutSnapshot.h"

#include <QImage>
#include <QPointF>
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace ktplace {

class LayoutView : public QWidget {
public:
    /// Lines from the selected cell to the others on its nets.
    struct NetOverlay {
        QPointF from;
        std::vector<QPointF> to;
    };

    explicit LayoutView(QWidget *parent = nullptr);

    /// Show @p snapshot. The first one, or one with a different die, is fitted
    /// to the window; otherwise the view stays where it is, so watching a stage
    /// rearrange the cells does not jump around.
    void setSnapshot(std::shared_ptr<const LayoutSnapshot> snapshot);
    [[nodiscard]] const std::shared_ptr<const LayoutSnapshot> &snapshot() const {
        return snapshot_;
    }

    void zoomFit();
    /// Zoom by @p factor (>1 in) about the window point @p anchor.
    void zoomBy(double factor, QPointF anchor);

    void setSelection(std::optional<std::size_t> cell);
    [[nodiscard]] std::optional<std::size_t> selection() const {
        return selected_;
    }
    void setNetOverlay(std::vector<NetOverlay> nets);

    [[nodiscard]] QPointF worldToScreen(QPointF world) const;
    [[nodiscard]] QPointF screenToWorld(QPointF screen) const;
    /// Pixels per design unit.
    [[nodiscard]] double scale() const {
        return scale_;
    }

    /// A click picked @p cell, or empty space (nullopt).
    std::function<void(std::optional<std::size_t>)> onSelect;
    /// The pointer is over design coordinate @p world (nullopt: it left).
    std::function<void(std::optional<QPointF>)> onCursor;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void render();
    void invalidate();

    std::shared_ptr<const LayoutSnapshot> snapshot_;
    QImage cache_;
    bool dirty_ = true;

    double scale_ = 1.0;  // pixels per design unit
    QPointF center_;      // design coordinate at the middle of the window

    std::optional<std::size_t> selected_;
    std::vector<NetOverlay> nets_;

    std::optional<QPointF> pressAt_;
    QPointF lastMouse_;
    bool panning_ = false;
};

}  // namespace ktplace
