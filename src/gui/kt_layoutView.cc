#include "gui/kt_layoutView.h"

#include "gui/kt_glCanvas.h"

#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

const QColor background(0x1e, 0x1e, 0x1e);
const QColor dieFill(0x26, 0x26, 0x26);
const QColor dieEdge(0x80, 0x80, 0x80);
const QColor rowLine(0x33, 0x33, 0x33);
const QColor movableFill(0x4a, 0x90, 0xd9);
const QColor movableEdge(0x8c, 0xc0, 0xf2);
const QColor macroFill(0x6e, 0x6e, 0x6e);
const QColor macroEdge(0xa8, 0xa8, 0xa8);
const QColor padFill(0xe0, 0x8a, 0x2a);
const QColor hint(0x9a, 0x9a, 0x9a);

constexpr double subPixelCell = 1.5;
constexpr double outlinedCell = 5.0;
constexpr double visibleRow = 5.0;
constexpr double minPad = 3.0;
constexpr double wheelStep = 1.25;
constexpr double fitMargin = 0.92;
constexpr double dragThreshold = 4.0;
constexpr std::size_t outlinedMacroLimit = 20000;

}  // namespace

class LayoutView::Impl {
public:
    explicit Impl(LayoutView &view) : view(view) {}

    LayoutView &view;
    GlCanvas *gpu = nullptr;
    LayoutFrame frame;
    QImage cache;
    bool dirty = true;
    double scale = 1.0;
    QPointF center;
    std::optional<QPointF> pressAt;
    QPointF lastMouse;
    bool panning = false;

    void invalidate() {
        if (gpu != nullptr) {
            gpu->setCamera(center, scale);
            return;
        }
        dirty = true;
        view.update();
    }

    [[nodiscard]] QRectF rectOf(double x, double y, double w, double h) const {
        return {view.worldToScreen({x, y + h}), QSizeF(w * scale, h * scale)};
    }

    void render();
};

void LayoutView::Impl::render() {
    const qreal dpr = view.devicePixelRatioF();
    const QSize pixels(std::max(1, static_cast<int>(view.width() * dpr)),
                       std::max(1, static_cast<int>(view.height() * dpr)));
    if (cache.size() != pixels) {
        cache = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
    }
    cache.setDevicePixelRatio(dpr);
    cache.fill(background);
    dirty = false;
    if (!frame.drawable()) {
        return;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const auto &die = geometry.die;

    QPainter painter(&cache);
    painter.fillRect(rectOf(die[0], die[1], die[2] - die[0], die[3] - die[1]), dieFill);

    const QPointF lo = view.screenToWorld({0.0, static_cast<double>(view.height())});
    const QPointF hi = view.screenToWorld({static_cast<double>(view.width()), 0.0});

    if (!geometry.rows.empty() && geometry.rows.front().height * scale >= visibleRow) {
        painter.setPen(QPen(rowLine, 0));
        for (const LayoutGeometry::Row &row : geometry.rows) {
            if (row.y > hi.y() || row.y + row.height < lo.y()) {
                continue;
            }
            const QRectF r = rectOf(row.xlo, row.y, row.xhi - row.xlo, row.height);
            painter.drawLine(r.bottomLeft(), r.bottomRight());
        }
    }

    const int pw = cache.width();
    const int ph = cache.height();
    const double pixelArea = scale * scale * dpr * dpr;
    std::vector<float> density;
    std::vector<QRectF> movable;
    std::vector<QRectF> macros;
    std::vector<QRectF> pads;
    for (std::size_t v = 0; v < frame.x.size(); ++v) {
        const double x = frame.x[v];
        const double y = frame.y[v];
        const double w = geometry.width[v];
        const double h = geometry.height[v];
        if (x > hi.x() || x + w < lo.x() || y > hi.y() || y + h < lo.y()) {
            continue;
        }
        const CellKind kind = geometry.kind[v];
        if (kind == CellKind::Pad) {
            QRectF r = rectOf(x, y, w, h);
            if (r.width() < minPad || r.height() < minPad) {
                r = QRectF(r.center() - QPointF(minPad / 2, minPad / 2), QSizeF(minPad, minPad));
            }
            pads.push_back(r);
            continue;
        }
        if (kind == CellKind::Movable && w * scale < subPixelCell && h * scale < subPixelCell) {
            if (density.empty()) {
                density.assign(static_cast<std::size_t>(pw) * static_cast<std::size_t>(ph), 0.0F);
            }
            const QPointF s = view.worldToScreen({x + w / 2.0, y + h / 2.0}) * dpr;
            const int ix = static_cast<int>(s.x());
            const int iy = static_cast<int>(s.y());
            if (ix >= 0 && iy >= 0 && ix < pw && iy < ph) {
                density[static_cast<std::size_t>(iy) * static_cast<std::size_t>(pw) +
                        static_cast<std::size_t>(ix)] += static_cast<float>(w * h * pixelArea);
            }
            continue;
        }
        (kind == CellKind::Macro ? macros : movable).push_back(rectOf(x, y, w, h));
    }

    painter.setPen(macros.size() < outlinedMacroLimit ? QPen(macroEdge, 0) : QPen(Qt::NoPen));
    painter.setBrush(macroFill);
    painter.drawRects(macros.data(), static_cast<int>(macros.size()));

    if (!density.empty()) {
        QImage overlay(pw, ph, QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        for (int py = 0; py < ph; ++py) {
            auto *line = reinterpret_cast<QRgb *>(overlay.scanLine(py));
            const float *sums = density.data() + static_cast<std::size_t>(py) * pw;
            for (int px = 0; px < pw; ++px) {
                if (sums[px] > 0.0F) {
                    const int alpha = static_cast<int>(std::min(1.0F, 0.25F + sums[px]) * 255.0F);
                    line[px] = qPremultiply(
                        qRgba(movableFill.red(), movableFill.green(), movableFill.blue(), alpha));
                }
            }
        }
        overlay.setDevicePixelRatio(dpr);
        painter.drawImage(QPointF(0.0, 0.0), overlay);
    }

    const bool outline = geometry.typicalWidth * scale >= outlinedCell;
    painter.setPen(outline ? QPen(movableEdge, 0) : QPen(Qt::NoPen));
    painter.setBrush(outline
                         ? QColor(movableFill.red(), movableFill.green(), movableFill.blue(), 170)
                         : movableFill);
    painter.drawRects(movable.data(), static_cast<int>(movable.size()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(padFill);
    painter.drawRects(pads.data(), static_cast<int>(pads.size()));

    painter.setPen(QPen(dieEdge, 0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rectOf(die[0], die[1], die[2] - die[0], die[3] - die[1]));
}

LayoutView::LayoutView(QWidget *parent) : QWidget(parent), pImpl(std::make_unique<Impl>(*this)) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (GlCanvas::available()) {
        pImpl->gpu = new GlCanvas(this);
    }
}

LayoutView::~LayoutView() = default;

void LayoutView::setFrame(LayoutFrame frame) {
    const bool refit = !pImpl->frame.drawable() || !frame.drawable() ||
                       pImpl->frame.geometry->die != frame.geometry->die;
    pImpl->frame = std::move(frame);
    if (pImpl->gpu != nullptr) {
        pImpl->gpu->setFrame(pImpl->frame);
    }
    if (refit) {
        zoomFit();
    } else {
        pImpl->invalidate();
    }
}

const LayoutFrame &LayoutView::frame() const {
    return pImpl->frame;
}

void LayoutView::zoomFit() {
    if (!pImpl->frame.drawable()) {
        pImpl->scale = 1.0;
        pImpl->center = {};
        pImpl->invalidate();
        return;
    }
    const auto &die = pImpl->frame.geometry->die;
    const double w = std::max(die[2] - die[0], 1e-9);
    const double h = std::max(die[3] - die[1], 1e-9);
    pImpl->scale = std::min(width() / w, height() / h) * fitMargin;
    if (!(pImpl->scale > 0.0) || !std::isfinite(pImpl->scale)) {
        pImpl->scale = 1.0;
    }
    pImpl->center = {(die[0] + die[2]) / 2.0, (die[1] + die[3]) / 2.0};
    pImpl->invalidate();
}

void LayoutView::zoomBy(double factor, QPointF anchor) {
    const QPointF before = screenToWorld(anchor);
    pImpl->scale = std::clamp(pImpl->scale * factor, 1e-9, 1e9);
    pImpl->center += before - screenToWorld(anchor);
    pImpl->invalidate();
}

QPointF LayoutView::worldToScreen(QPointF world) const {
    return {(world.x() - pImpl->center.x()) * pImpl->scale + width() / 2.0,
            height() / 2.0 - (world.y() - pImpl->center.y()) * pImpl->scale};
}

QPointF LayoutView::screenToWorld(QPointF screen) const {
    return {(screen.x() - width() / 2.0) / pImpl->scale + pImpl->center.x(),
            (height() / 2.0 - screen.y()) / pImpl->scale + pImpl->center.y()};
}

void LayoutView::paintEvent(QPaintEvent *) {
    if (pImpl->gpu != nullptr) {
        return;
    }
    if (pImpl->dirty) {
        pImpl->render();
    }
    QPainter painter(this);
    painter.drawImage(0, 0, pImpl->cache);
    if (!pImpl->frame.drawable()) {
        painter.setPen(hint);
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No design loaded\n\nFile > Open Design, then Run"));
    }
}

void LayoutView::resizeEvent(QResizeEvent *) {
    if (pImpl->gpu != nullptr) {
        pImpl->gpu->setGeometry(rect());
    }
    if (pImpl->frame.drawable() && pImpl->cache.isNull() && pImpl->gpu == nullptr) {
        zoomFit();
    } else {
        pImpl->invalidate();
    }
}

void LayoutView::wheelEvent(QWheelEvent *event) {
    const double notches = event->angleDelta().y() / 120.0;
    if (notches != 0.0) {
        zoomBy(std::pow(wheelStep, notches), event->position());
    }
    event->accept();
}

void LayoutView::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        pImpl->pressAt = event->position();
        pImpl->lastMouse = event->position();
        pImpl->panning = event->button() == Qt::MiddleButton;
    }
}

void LayoutView::mouseMoveEvent(QMouseEvent *event) {
    if (!pImpl->pressAt) {
        return;
    }
    if (!pImpl->panning &&
        (event->position() - *pImpl->pressAt).manhattanLength() > dragThreshold) {
        pImpl->panning = true;
        setCursor(Qt::ClosedHandCursor);
    }
    if (pImpl->panning) {
        const QPointF delta = event->position() - pImpl->lastMouse;
        pImpl->center -= QPointF(delta.x() / pImpl->scale, -delta.y() / pImpl->scale);
        pImpl->lastMouse = event->position();
        pImpl->invalidate();
    }
}

void LayoutView::mouseReleaseEvent(QMouseEvent *) {
    pImpl->pressAt.reset();
    pImpl->panning = false;
    unsetCursor();
}

void LayoutView::keyPressEvent(QKeyEvent *event) {
    const QPointF middle(width() / 2.0, height() / 2.0);
    switch (event->key()) {
        case Qt::Key_F:
            zoomFit();
            break;
        case Qt::Key_Plus:
        case Qt::Key_Equal:
            zoomBy(wheelStep * wheelStep, middle);
            break;
        case Qt::Key_Minus:
            zoomBy(1.0 / (wheelStep * wheelStep), middle);
            break;
        default:
            QWidget::keyPressEvent(event);
    }
}

}  // namespace ktplace::gui
