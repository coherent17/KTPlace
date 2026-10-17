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
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

const QColor background(0x10, 0x14, 0x18);
const QColor dieFill(0x10, 0x14, 0x18);
const QColor dieEdge(0x8b, 0x97, 0xa3);
const QColor rowLine(0x3b, 0x45, 0x4e);
const QColor movableFill(0x4a, 0xb5, 0xe5);
const QColor movableEdge(0x35, 0x82, 0xa5);
const QColor macroFill(0xd9, 0x4d, 0x4a);
const QColor macroEdge(0x9c, 0x37, 0x35);
const QColor padFill(0xd9, 0x4d, 0x4a);
const QColor hint(0x9a, 0x9a, 0x9a);
const QColor selection(0xff, 0xd5, 0x4f);
const QColor cardFill(0x10, 0x14, 0x18, 0xe6);
const QColor cardText(0xe6, 0xed, 0xf3);

constexpr double subPixelCell = 1.5;
constexpr double outlinedCell = 5.0;
constexpr double visibleRow = 5.0;
constexpr double minPad = 3.0;
constexpr double wheelStep = 1.25;
constexpr double fitMargin = 0.92;
constexpr double dragThreshold = 4.0;
constexpr std::size_t outlinedMacroLimit = 20000;
constexpr std::uint32_t maxFlyLinePins = 1000;
constexpr double pickSlackPx = 3.0;
constexpr double pinMarkPx = 4.0;

class SelectionLayer : public QWidget {
public:
    SelectionLayer(QWidget *parent, std::function<void(QPainter &)> paint)
        : QWidget(parent), paint(std::move(paint)) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        paint(painter);
    }

private:
    std::function<void(QPainter &)> paint;
};

}  // namespace

class LayoutView::Impl {
public:
    explicit Impl(LayoutView &view) : view(view) {}

    LayoutView &view;
    GlCanvas *gpu = nullptr;
    QWidget *layer = nullptr;
    LayoutFrame frame;
    QImage cache;
    bool dirty = true;
    double scale = 1.0;
    QPointF center;
    std::optional<QPointF> pressAt;
    QPointF lastMouse;
    bool panning = false;
    std::optional<std::uint32_t> selected;

    void invalidate() {
        if (gpu != nullptr) {
            gpu->setCamera(center, scale);
            layer->update();
            return;
        }
        dirty = true;
        view.update();
    }

    [[nodiscard]] QRectF rectOf(double x, double y, double w, double h) const {
        return {view.worldToScreen({x, y + h}), QSizeF(w * scale, h * scale)};
    }

    void render();
    void refreshOverlay() const;
    [[nodiscard]] std::optional<std::uint32_t> pick(QPointF screen) const;
    void paintSelection(QPainter &painter) const;
};

void LayoutView::Impl::refreshOverlay() const {
    if (layer != nullptr) {
        layer->update();
    } else {
        view.update();
    }
}

std::optional<std::uint32_t> LayoutView::Impl::pick(QPointF screen) const {
    if (!frame.drawable()) {
        return std::nullopt;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const QPointF at = view.screenToWorld(screen);
    const double slack = pickSlackPx / scale;
    const auto layer = [](CellKind kind) {
        return kind == CellKind::Pad ? 0 : (kind == CellKind::Movable ? 1 : 2);
    };
    std::optional<std::uint32_t> best;
    int bestLayer = 3;
    double bestDistance = 0.0;
    for (std::uint32_t v = 0; v < frame.x.size(); ++v) {
        const double x0 = frame.x[v];
        const double y0 = frame.y[v];
        const double x1 = x0 + geometry.width[v];
        const double y1 = y0 + geometry.height[v];
        if (at.x() < x0 - slack || at.x() > x1 + slack || at.y() < y0 - slack ||
            at.y() > y1 + slack) {
            continue;
        }
        const int cellLayer = layer(geometry.kind[v]);
        const double dx = at.x() - (x0 + x1) / 2.0;
        const double dy = at.y() - (y0 + y1) / 2.0;
        const double distance = dx * dx + dy * dy;
        if (cellLayer < bestLayer || (cellLayer == bestLayer && distance < bestDistance)) {
            best = v;
            bestLayer = cellLayer;
            bestDistance = distance;
        }
    }
    return best;
}

void LayoutView::Impl::paintSelection(QPainter &painter) const {
    if (!selected || !frame.drawable() || *selected >= frame.x.size()) {
        return;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const std::uint32_t cell = *selected;
    const auto pinAt = [&](std::uint32_t pin) {
        const std::uint32_t owner = geometry.pinCell[pin];
        return view.worldToScreen(
            {frame.x[owner] + geometry.pinDx[pin], frame.y[owner] + geometry.pinDy[pin]});
    };
    painter.setRenderHint(QPainter::Antialiasing);

    QColor boxFill = selection;
    boxFill.setAlpha(0x1c);
    QColor boxEdge = selection;
    boxEdge.setAlpha(0x70);
    QColor flyLine = selection;
    flyLine.setAlpha(0xc0);
    std::size_t pins = 0;
    const std::uint32_t netsBegin = geometry.cellNetStart[cell];
    const std::uint32_t netsEnd = geometry.cellNetStart[cell + 1];
    for (std::uint32_t k = netsBegin; k < netsEnd; ++k) {
        const std::uint32_t net = geometry.cellNet[k];
        const std::uint32_t first = geometry.netPinStart[net];
        const std::uint32_t last = geometry.netPinStart[net + 1];
        QRectF box;
        std::optional<QPointF> own;
        for (std::uint32_t p = first; p < last; ++p) {
            const QPointF at = pinAt(p);
            box = p == first ? QRectF(at, at) : box.united(QRectF(at, at));
            if (geometry.pinCell[p] == cell) {
                ++pins;
                if (!own) {
                    own = at;
                }
            }
        }
        painter.setPen(QPen(boxEdge, 0));
        painter.setBrush(boxFill);
        painter.drawRect(box);
        if (!own || last - first > maxFlyLinePins) {
            continue;
        }
        painter.setPen(QPen(flyLine, 1.2));
        for (std::uint32_t p = first; p < last; ++p) {
            if (geometry.pinCell[p] != cell) {
                painter.drawLine(*own, pinAt(p));
            }
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(selection);
        for (std::uint32_t p = first; p < last; ++p) {
            painter.drawRect(QRectF(pinAt(p) - QPointF(pinMarkPx / 2, pinMarkPx / 2),
                                    QSizeF(pinMarkPx, pinMarkPx)));
        }
    }

    painter.setPen(QPen(selection, 2.0));
    painter.setBrush(Qt::NoBrush);
    QRectF outline =
        rectOf(frame.x[cell], frame.y[cell], geometry.width[cell], geometry.height[cell]);
    if (outline.width() < pinMarkPx || outline.height() < pinMarkPx) {
        outline = QRectF(outline.center() - QPointF(pinMarkPx, pinMarkPx),
                         QSizeF(2 * pinMarkPx, 2 * pinMarkPx));
    }
    painter.drawRect(outline);

    const char *kindName = geometry.kind[cell] == CellKind::Movable ? "movable cell"
                           : geometry.kind[cell] == CellKind::Macro ? "fixed macro"
                                                                    : "I/O pad";
    const QStringList lines{
        QString::fromStdString(geometry.name[cell]),
        QStringLiteral("%1, %2 x %3")
            .arg(QString::fromUtf8(kindName))
            .arg(geometry.width[cell])
            .arg(geometry.height[cell]),
        QStringLiteral("at (%1, %2)").arg(frame.x[cell], 0, 'f', 1).arg(frame.y[cell], 0, 'f', 1),
        QStringLiteral("%1 pins on %2 nets").arg(pins).arg(netsEnd - netsBegin)};
    const QFontMetrics metrics = painter.fontMetrics();
    int widest = 0;
    for (const QString &line : lines) {
        widest = std::max(widest, metrics.horizontalAdvance(line));
    }
    const int pad = 8;
    const QRect card(10, 10, widest + 2 * pad,
                     static_cast<int>(lines.size()) * metrics.lineSpacing() + 2 * pad);
    painter.setPen(QPen(selection, 1.0));
    painter.setBrush(cardFill);
    painter.drawRoundedRect(card, 4, 4);
    painter.setPen(cardText);
    for (int i = 0; i < lines.size(); ++i) {
        painter.drawText(card.left() + pad,
                         card.top() + pad + metrics.ascent() + i * metrics.lineSpacing(), lines[i]);
    }
}

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
        pImpl->layer = new SelectionLayer(this, [impl = pImpl.get()](QPainter &painter) {
            impl->paintSelection(painter);
        });
        pImpl->layer->raise();
    }
}

LayoutView::~LayoutView() = default;

void LayoutView::setFrame(LayoutFrame frame) {
    const bool refit = !pImpl->frame.drawable() || !frame.drawable() ||
                       pImpl->frame.geometry->die != frame.geometry->die;
    if (!frame.drawable() || frame.geometry != pImpl->frame.geometry) {
        pImpl->selected.reset();
    }
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

void LayoutView::selectAt(QPointF screen) {
    pImpl->selected = pImpl->pick(screen);
    pImpl->refreshOverlay();
}

std::optional<std::uint32_t> LayoutView::selectedCell() const {
    return pImpl->selected;
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
    pImpl->paintSelection(painter);
    if (!pImpl->frame.drawable()) {
        painter.setPen(hint);
        painter.drawText(rect(), Qt::AlignCenter,
                         QStringLiteral("No design loaded\n\nFile > Open Design, then Run"));
    }
}

void LayoutView::resizeEvent(QResizeEvent *) {
    if (pImpl->gpu != nullptr) {
        pImpl->gpu->setGeometry(rect());
        pImpl->layer->setGeometry(rect());
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

void LayoutView::mouseReleaseEvent(QMouseEvent *event) {
    if (pImpl->pressAt && !pImpl->panning && event->button() == Qt::LeftButton) {
        selectAt(event->position());
    }
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
