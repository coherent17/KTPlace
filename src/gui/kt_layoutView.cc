// @file kt_layoutView.cc
// Implementation of LayoutView.

#include "gui/kt_layoutView.h"

#include "util/kt_log.h"

#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace ktplace {

namespace {

const QColor kBackground(0x1e, 0x1e, 0x1e);
const QColor kDieFill(0x26, 0x26, 0x26);
const QColor kDieEdge(0x80, 0x80, 0x80);
const QColor kRow(0x33, 0x33, 0x33);
const QColor kMovable(0x4a, 0x90, 0xd9);
const QColor kMovableEdge(0x8c, 0xc0, 0xf2);
const QColor kFixed(0x6e, 0x6e, 0x6e);
const QColor kFixedEdge(0xa8, 0xa8, 0xa8);
const QColor kTerminal(0xe0, 0x8a, 0x2a);
const QColor kSelected(0xff, 0xd6, 0x3a);
const QColor kNet(0xff, 0xd6, 0x3a, 0x90);

// Below this many pixels on a side a cell is summed into the density map.
constexpr double kSubPixel = 1.5;
// From this many pixels on a side a cell gets an outline of its own.
constexpr double kOutlined = 5.0;
// Rows are drawn once they are at least this many pixels tall.
constexpr double kRowsVisible = 5.0;
// Pads are drawn at least this big, so they stay findable when zoomed out.
constexpr double kMinPad = 3.0;

constexpr double kWheelStep = 1.25;

}  // namespace

LayoutView::LayoutView(QWidget *parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

void LayoutView::setSnapshot(std::shared_ptr<const LayoutSnapshot> snapshot) {
    const bool refit = !snapshot_ || !snapshot || snapshot_->die != snapshot->die;
    snapshot_ = std::move(snapshot);
    if (selected_ && (!snapshot_ || *selected_ >= snapshot_->cells.size())) {
        selected_.reset();
        nets_.clear();
    }
    if (refit) {
        zoomFit();
    } else {
        invalidate();
    }
}

void LayoutView::zoomFit() {
    if (!snapshot_) {
        scale_ = 1.0;
        center_ = {};
        invalidate();
        return;
    }
    const auto &d = snapshot_->die;
    const double w = std::max(d[2] - d[0], 1e-9);
    const double h = std::max(d[3] - d[1], 1e-9);
    // A margin of 4% on each side, so the die edge is visible.
    scale_ = std::min(width() / w, height() / h) * 0.92;
    if (!(scale_ > 0.0) || !std::isfinite(scale_)) {
        scale_ = 1.0;
    }
    center_ = {(d[0] + d[2]) / 2.0, (d[1] + d[3]) / 2.0};
    invalidate();
}

void LayoutView::zoomBy(double factor, QPointF anchor) {
    // Keep the design point under the anchor where it is.
    const QPointF before = screenToWorld(anchor);
    scale_ = std::clamp(scale_ * factor, 1e-9, 1e9);
    const QPointF after = screenToWorld(anchor);
    center_ += before - after;
    invalidate();
}

void LayoutView::setSelection(std::optional<std::size_t> cell) {
    selected_ = cell;
    if (!cell) {
        nets_.clear();
    }
    update();
}

void LayoutView::setNetOverlay(std::vector<NetOverlay> nets) {
    nets_ = std::move(nets);
    update();
}

QPointF LayoutView::worldToScreen(QPointF world) const {
    return {(world.x() - center_.x()) * scale_ + width() / 2.0,
            height() / 2.0 - (world.y() - center_.y()) * scale_};
}

QPointF LayoutView::screenToWorld(QPointF screen) const {
    return {(screen.x() - width() / 2.0) / scale_ + center_.x(),
            (height() / 2.0 - screen.y()) / scale_ + center_.y()};
}

void LayoutView::invalidate() {
    dirty_ = true;
    update();
}

void LayoutView::render() {
    QElapsedTimer timer;
    timer.start();
    const qreal dpr = devicePixelRatioF();
    const QSize pixels(std::max(1, static_cast<int>(width() * dpr)),
                       std::max(1, static_cast<int>(height() * dpr)));
    if (cache_.size() != pixels) {
        cache_ = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
    }
    cache_.setDevicePixelRatio(dpr);
    cache_.fill(kBackground);
    dirty_ = false;
    if (!snapshot_) {
        return;
    }
    const LayoutSnapshot &snap = *snapshot_;

    QPainter p(&cache_);
    const auto rectOf = [&](double x, double y, double w, double h) {
        const QPointF a = worldToScreen({x, y + h});
        return QRectF(a, QSizeF(w * scale_, h * scale_));
    };

    const auto &d = snap.die;
    p.fillRect(rectOf(d[0], d[1], d[2] - d[0], d[3] - d[1]), kDieFill);

    // The visible part of the design, to skip what is off screen.
    const QPointF lo = screenToWorld({0.0, static_cast<double>(height())});
    const QPointF hi = screenToWorld({static_cast<double>(width()), 0.0});

    if (!snap.rows.empty() && snap.rows.front().h * scale_ >= kRowsVisible) {
        p.setPen(QPen(kRow, 0));
        for (const LayoutSnapshot::Row &r : snap.rows) {
            if (r.y > hi.y() || r.y + r.h < lo.y()) {
                continue;
            }
            const QRectF rr = rectOf(r.xlo, r.y, r.xhi - r.xlo, r.h);
            p.drawLine(rr.bottomLeft(), rr.bottomRight());
        }
    }

    // Sub-pixel cells are summed per device pixel, in units of "pixels covered".
    const int pw = cache_.width();
    const int ph = cache_.height();
    std::vector<float> density;
    std::vector<QRectF> movable;
    std::vector<QRectF> fixed;
    std::vector<QRectF> pads;
    const double pixelArea = scale_ * scale_ * dpr * dpr;
    for (const LayoutSnapshot::Cell &c : snap.cells) {
        if (c.x > hi.x() || c.x + c.w < lo.x() || c.y > hi.y() || c.y + c.h < lo.y()) {
            continue;
        }
        if (c.kind == LayoutSnapshot::Kind::Terminal) {
            QRectF r = rectOf(c.x, c.y, c.w, c.h);
            if (r.width() < kMinPad || r.height() < kMinPad) {
                r = QRectF(r.center() - QPointF(kMinPad / 2, kMinPad / 2),
                           QSizeF(kMinPad, kMinPad));
            }
            pads.push_back(r);
            continue;
        }
        const double wpx = c.w * scale_;
        const double hpx = c.h * scale_;
        if (c.kind == LayoutSnapshot::Kind::Movable && wpx < kSubPixel && hpx < kSubPixel) {
            if (density.empty()) {
                density.assign(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph), 0.0F);
            }
            const QPointF s = worldToScreen({c.x + c.w / 2.0, c.y + c.h / 2.0}) * dpr;
            const int ix = static_cast<int>(s.x());
            const int iy = static_cast<int>(s.y());
            if (ix >= 0 && iy >= 0 && ix < pw && iy < ph) {
                density[static_cast<std::size_t>(iy) * static_cast<std::size_t>(pw) +
                        static_cast<std::size_t>(ix)] +=
                    static_cast<float>(static_cast<double>(c.w) * c.h * pixelArea);
            }
            continue;
        }
        (c.kind == LayoutSnapshot::Kind::Fixed ? fixed : movable)
            .push_back(rectOf(c.x, c.y, c.w, c.h));
    }

    // Macros first, so standard cells over them stay visible.
    p.setPen(fixed.size() < 20000 ? QPen(kFixedEdge, 0) : Qt::NoPen);
    p.setBrush(kFixed);
    p.drawRects(fixed.data(), static_cast<int>(fixed.size()));

    if (!density.empty()) {
        // Full coverage is full colour; partial coverage fades into the die.
        QImage overlay(pw, ph, QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        for (int y = 0; y < ph; ++y) {
            auto *line = reinterpret_cast<QRgb *>(overlay.scanLine(y));
            const float *row = density.data() + static_cast<std::size_t>(y) * pw;
            for (int x = 0; x < pw; ++x) {
                if (row[x] > 0.0F) {
                    const int a = static_cast<int>(std::min(1.0F, 0.25F + row[x]) * 255.0F);
                    line[x] =
                        qPremultiply(qRgba(kMovable.red(), kMovable.green(), kMovable.blue(), a));
                }
            }
        }
        // In device pixels, like the cache it is drawn onto.
        overlay.setDevicePixelRatio(dpr);
        p.drawImage(QPointF(0.0, 0.0), overlay);
    }

    const bool outline = snap.typicalWidth * scale_ >= kOutlined;
    p.setPen(outline ? QPen(kMovableEdge, 0) : Qt::NoPen);
    p.setBrush(outline ? QColor(kMovable.red(), kMovable.green(), kMovable.blue(), 170) : kMovable);
    p.drawRects(movable.data(), static_cast<int>(movable.size()));

    p.setPen(Qt::NoPen);
    p.setBrush(kTerminal);
    p.drawRects(pads.data(), static_cast<int>(pads.size()));

    p.setPen(QPen(kDieEdge, 0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(rectOf(d[0], d[1], d[2] - d[0], d[3] - d[1]));
    ktlog.trace("layout view: {} cells drawn as rectangles, the rest as density, in {} ms",
                movable.size() + fixed.size() + pads.size(), timer.elapsed());
}

void LayoutView::paintEvent(QPaintEvent *) {
    if (dirty_) {
        render();
    }
    QPainter p(this);
    p.drawImage(0, 0, cache_);

    if (!snapshot_) {
        p.setPen(QColor(0x9a, 0x9a, 0x9a));
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("No design loaded\n\nFile > Open Design, or `read_design <dir>` "
                                  "in the Tcl Console"));
        return;
    }
    if (!selected_) {
        return;
    }
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(kNet, 1.0));
    for (const NetOverlay &n : nets_) {
        const QPointF a = worldToScreen(n.from);
        for (const QPointF &t : n.to) {
            p.drawLine(a, worldToScreen(t));
        }
    }
    const LayoutSnapshot::Cell &c = snapshot_->cells[*selected_];
    QRectF r(worldToScreen({c.x, static_cast<double>(c.y) + c.h}),
             QSizeF(c.w * scale_, c.h * scale_));
    // Never smaller than a few pixels, so the selection can be found.
    if (r.width() < 7.0 || r.height() < 7.0) {
        r = QRectF(r.center() - QPointF(3.5, 3.5), QSizeF(7.0, 7.0));
    }
    p.setPen(QPen(kSelected, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

void LayoutView::resizeEvent(QResizeEvent *) {
    if (snapshot_ && cache_.isNull()) {
        zoomFit();
    } else {
        invalidate();
    }
}

void LayoutView::wheelEvent(QWheelEvent *event) {
    const double notches = event->angleDelta().y() / 120.0;
    if (notches != 0.0) {
        zoomBy(std::pow(kWheelStep, notches), event->position());
    }
    event->accept();
}

void LayoutView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        pressAt_ = event->position();
        lastMouse_ = event->position();
        panning_ = event->button() == Qt::MiddleButton;
    }
}

void LayoutView::mouseMoveEvent(QMouseEvent *event) {
    if (onCursor) {
        onCursor(snapshot_ ? std::optional<QPointF>(screenToWorld(event->position()))
                           : std::nullopt);
    }
    if (!pressAt_) {
        return;
    }
    // A drag past a few pixels is a pan, not a click.
    if (!panning_ && (event->position() - *pressAt_).manhattanLength() > 4.0) {
        panning_ = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (panning_) {
        const QPointF delta = event->position() - lastMouse_;
        center_ -= QPointF(delta.x() / scale_, -delta.y() / scale_);
        lastMouse_ = event->position();
        invalidate();
    }
}

void LayoutView::mouseReleaseEvent(QMouseEvent *event) {
    if (!pressAt_) {
        return;
    }
    const bool click = !panning_ && event->button() == Qt::LeftButton;
    pressAt_.reset();
    panning_ = false;
    unsetCursor();
    if (!click || !snapshot_) {
        return;
    }
    const QPointF w = screenToWorld(event->position());
    std::optional<std::size_t> hit = snapshot_->cellAt(w.x(), w.y());
    if (!hit) {
        // Tiny cells are hard to hit exactly: try a few pixels around the click.
        const double slack = 3.0 / scale_;
        for (const QPointF off :
             {QPointF(slack, 0), QPointF(-slack, 0), QPointF(0, slack), QPointF(0, -slack)}) {
            if ((hit = snapshot_->cellAt(w.x() + off.x(), w.y() + off.y()))) {
                break;
            }
        }
    }
    setSelection(hit);
    if (onSelect) {
        onSelect(hit);
    }
}

void LayoutView::leaveEvent(QEvent *) {
    if (onCursor) {
        onCursor(std::nullopt);
    }
}

void LayoutView::keyPressEvent(QKeyEvent *event) {
    const QPointF middle(width() / 2.0, height() / 2.0);
    switch (event->key()) {
        case Qt::Key_F:
            zoomFit();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomBy(kWheelStep * kWheelStep, middle);
            break;
        case Qt::Key_Minus:
            zoomBy(1.0 / (kWheelStep * kWheelStep), middle);
            break;
        case Qt::Key_Escape:
            setSelection(std::nullopt);
            if (onSelect) {
                onSelect(std::nullopt);
            }
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

}  // namespace ktplace
