#include "gui/kt_layoutView.h"

#include "gui/kt_congestion.h"
#include "gui/kt_glCanvas.h"

#include <QActionGroup>
#include <QContextMenuEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

const QColor background(0x10, 0x14, 0x18);
const QColor hint(0x9a, 0x9a, 0x9a);
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
constexpr std::size_t maxDetailedNets = 2000;
constexpr std::size_t maxBoxedNets = 16;
constexpr std::size_t thinOutlineCells = 1000;
constexpr double pickSlackPx = 3.0;
constexpr double pinMarkPx = 4.0;
constexpr double minZoomCells = 20.0;
constexpr int edgeDarkening = 140;
constexpr int fenceFillAlpha = 0x26;
constexpr int boxFillAlpha = 0x1c;
constexpr int boxEdgeAlpha = 0x70;
constexpr int flyLineAlpha = 0xc0;
constexpr std::array<std::size_t, 4> congestionGrids{64, 128, 256, 512};

QRgb getHeatColor(float value) {
    const std::array<std::uint8_t, 4> c = getDemandColour(value);
    return qPremultiply(qRgba(c[0], c[1], c[2], c[3]));
}

QColor withAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

class OverlayLayer : public QWidget {
public:
    OverlayLayer(QWidget *parent, std::function<void(QPainter &)> painter)
        : QWidget(parent), paint(std::move(painter)) {
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
    explicit Impl(LayoutView &owner) : view(owner) {}

    LayoutView &view;
    GlCanvas *gpu = nullptr;
    QWidget *layer = nullptr;
    LayoutFrame frame;
    SelectionSet selection;
    DisplayStyle style;
    QImage cache;
    bool dirty = true;
    double scale = 1.0;
    QPointF center;
    std::optional<QPointF> pressAt;
    QPointF lastMouse;
    bool panning = false;
    std::optional<QRectF> rubberBand;
    std::size_t congestionBins = 256;
    std::optional<DemandMap> demand;
    QImage heat;
    std::function<void(const DisplayStyle &)> styleListener;
    std::function<void(std::optional<QPointF>)> cursorListener;

    void invalidate() {
        if (gpu != nullptr) {
            gpu->setCamera(center, scale);
            layer->update();
            return;
        }
        dirty = true;
        view.update();
    }

    [[nodiscard]] QRectF getRectOf(double x, double y, double w, double h) const {
        return {view.getWorldToScreen({x, y + h}), QSizeF(w * scale, h * scale)};
    }

    [[nodiscard]] QRectF getRectOf(const BBox &box) const {
        return getRectOf(box.lowerLeft.x, box.lowerLeft.y, box.upperRight.x - box.lowerLeft.x,
                         box.upperRight.y - box.lowerLeft.y);
    }

    [[nodiscard]] QPointF getPinOnScreen(std::uint32_t pin) const {
        const LayoutGeometry &geometry = *frame.geometry;
        const std::uint32_t owner = geometry.pinCell[pin];
        return view.getWorldToScreen(
            {frame.x[owner] + geometry.pinDx[pin], frame.y[owner] + geometry.pinDy[pin]});
    }

    [[nodiscard]] QPointF getFlyLineAnchor(std::uint32_t firstPin, std::uint32_t lastPin) const {
        const LayoutGeometry &geometry = *frame.geometry;
        QPointF centroid;
        for (std::uint32_t pin = firstPin; pin < lastPin; ++pin) {
            if (selection.contains(geometry.pinCell[pin])) {
                return getPinOnScreen(pin);
            }
            centroid += getPinOnScreen(pin);
        }
        return centroid / static_cast<double>(lastPin - firstPin);
    }

    void render();
    void refreshOverlay() const;
    void applyStyle(const DisplayStyle &newStyle, bool announce);
    [[nodiscard]] std::optional<std::uint32_t> pick(QPointF screen) const;
    void updateCongestion();
    void paintOverlay(QPainter &painter) const;
    void paintFences(QPainter &painter) const;
    void paintCongestion(QPainter &painter) const;
    void paintMarkedNets(QPainter &painter) const;
    void paintSelection(QPainter &painter) const;
    void paintRubberBand(QPainter &painter) const;
};

void LayoutView::Impl::applyStyle(const DisplayStyle &newStyle, bool announce) {
    const bool demandChanged =
        newStyle.isShown(DisplayLayer::RoutingDemand) != style.isShown(DisplayLayer::RoutingDemand);
    style = newStyle;
    if (demandChanged) {
        updateCongestion();
    }
    if (gpu != nullptr) {
        gpu->setStyle(style);
    }
    dirty = true;
    view.update();
    refreshOverlay();
    if (announce && styleListener) {
        styleListener(style);
    }
}

void LayoutView::Impl::updateCongestion() {
    if (!style.isShown(DisplayLayer::RoutingDemand) || !frame.isDrawable()) {
        demand.reset();
        heat = QImage();
        return;
    }
    demand = estimateDemand(*frame.geometry, frame.x, frame.y, congestionBins);
    const DemandMap &map = *demand;
    heat = QImage(static_cast<int>(map.nx), static_cast<int>(map.ny),
                  QImage::Format_ARGB32_Premultiplied);
    const float top = map.p99 > 0.0F ? map.p99 : std::max(map.peak, 1e-9F);
    for (std::size_t iy = 0; iy < map.ny; ++iy) {
        auto *line = reinterpret_cast<QRgb *>(heat.scanLine(static_cast<int>(map.ny - 1 - iy)));
        for (std::size_t ix = 0; ix < map.nx; ++ix) {
            const float value = map.at(ix, iy);
            line[ix] = value > 0.0F ? getHeatColor(value / top) : qRgba(0, 0, 0, 0);
        }
    }
}

void LayoutView::Impl::paintOverlay(QPainter &painter) const {
    if (frame.isDrawable()) {
        paintFences(painter);
        paintCongestion(painter);
        paintMarkedNets(painter);
        paintSelection(painter);
    }
    paintRubberBand(painter);
}

void LayoutView::Impl::paintFences(QPainter &painter) const {
    const std::vector<LayoutGeometry::Fence> &fences = frame.geometry->fences;
    if (fences.empty() || !style.isShown(DisplayLayer::Fences)) {
        return;
    }
    const QColor color = style[DisplayLayer::Fences].color;
    painter.setPen(QPen(color, 1.0));
    painter.setBrush(withAlpha(color, fenceFillAlpha));
    for (const LayoutGeometry::Fence &fence : fences) {
        for (const BBox &rect : fence.rects) {
            painter.drawRect(getRectOf(rect));
        }
        if (fence.rects.empty()) {
            continue;
        }
        const QRectF first = getRectOf(fence.rects.front());
        if (first.width() >
                painter.fontMetrics().horizontalAdvance(QString::fromStdString(fence.name)) &&
            first.height() > painter.fontMetrics().height()) {
            painter.drawText(first.adjusted(4, 2, -4, -2), Qt::AlignLeft | Qt::AlignTop,
                             QString::fromStdString(fence.name));
        }
    }
}

void LayoutView::Impl::paintCongestion(QPainter &painter) const {
    if (!demand || heat.isNull()) {
        return;
    }
    const DemandMap &map = *demand;
    const QPointF topLeft =
        view.getWorldToScreen({map.x0, map.y0 + static_cast<double>(map.ny) * map.binH});
    const QPointF bottomRight =
        view.getWorldToScreen({map.x0 + static_cast<double>(map.nx) * map.binW, map.y0});
    painter.drawImage(QRectF(topLeft, bottomRight), heat);

    const std::size_t peakX = map.peakBin % map.nx;
    const std::size_t peakY = map.peakBin / map.nx;
    const QStringList lines{
        QStringLiteral("Routing demand: net bounding boxes, 1/(w*h) each"),
        QStringLiteral("grid %1 x %2, bin %3, %4 nets")
            .arg(map.nx)
            .arg(map.ny)
            .arg(map.binW, 0, 'f', 1)
            .arg(map.nets),
        QStringLiteral("peak %1 nets/bin at (%2, %3)")
            .arg(map.peak, 0, 'f', 2)
            .arg(map.x0 + (static_cast<double>(peakX) + 0.5) * map.binW, 0, 'f', 0)
            .arg(map.y0 + (static_cast<double>(peakY) + 0.5) * map.binH, 0, 'f', 0),
        QStringLiteral("mean %1, 99th pct %2, %3% of bins over 2x mean")
            .arg(map.mean, 0, 'f', 2)
            .arg(map.p99, 0, 'f', 2)
            .arg(100.0 * map.hotShare, 0, 'f', 1)};
    const QFontMetrics metrics = painter.fontMetrics();
    int widest = 0;
    for (const QString &line : lines) {
        widest = std::max(widest, metrics.horizontalAdvance(line));
    }
    const int pad = 8;
    const int barH = 10;
    const int height = static_cast<int>(lines.size()) * metrics.lineSpacing() + barH + 3 * pad;
    const QRect card(10, view.height() - height - 10, widest + 2 * pad, height);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(style[DisplayLayer::Die].color, 1.0));
    painter.setBrush(cardFill);
    painter.drawRoundedRect(card, 4, 4);
    painter.setPen(cardText);
    for (int i = 0; i < lines.size(); ++i) {
        painter.drawText(card.left() + pad,
                         card.top() + pad + metrics.ascent() + i * metrics.lineSpacing(), lines[i]);
    }
    const int barTop = card.bottom() - pad - barH;
    const int barWidth = card.width() - 2 * pad;
    for (int i = 0; i < barWidth; ++i) {
        painter.fillRect(card.left() + pad + i, barTop, 1, barH,
                         QColor::fromRgba(qUnpremultiply(getHeatColor(
                             static_cast<float>(i) / static_cast<float>(barWidth - 1)))));
    }
}

void LayoutView::Impl::paintMarkedNets(QPainter &painter) const {
    const std::vector<std::uint32_t> &nets = selection.getNets();
    if (nets.empty() || !style.isShown(DisplayLayer::MarkedNets)) {
        return;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const QColor color = style[DisplayLayer::MarkedNets].color;
    const bool detailed = nets.size() <= maxDetailedNets;
    const bool boxed = nets.size() <= maxBoxedNets;
    painter.setRenderHint(QPainter::Antialiasing, detailed);
    for (const std::uint32_t net : nets) {
        const std::optional<BBox> box = frame.getNetBounds(net);
        if (!box) {
            continue;
        }
        if (boxed || !detailed) {
            painter.setPen(QPen(withAlpha(color, boxEdgeAlpha), 0));
            painter.setBrush(boxed ? withAlpha(color, boxFillAlpha) : QColor(Qt::transparent));
            painter.drawRect(getRectOf(*box));
        }
        const std::uint32_t first = geometry.netPinStart[net];
        const std::uint32_t last = geometry.netPinStart[net + 1];
        if (!detailed || last - first > maxFlyLinePins) {
            continue;
        }
        const QPointF anchor = getFlyLineAnchor(first, last);
        painter.setPen(QPen(withAlpha(color, flyLineAlpha), 1.2));
        for (std::uint32_t pin = first; pin < last; ++pin) {
            painter.drawLine(anchor, getPinOnScreen(pin));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        for (std::uint32_t pin = first; pin < last; ++pin) {
            painter.drawRect(QRectF(getPinOnScreen(pin) - QPointF(pinMarkPx / 2, pinMarkPx / 2),
                                    QSizeF(pinMarkPx, pinMarkPx)));
        }
    }
}

void LayoutView::Impl::paintSelection(QPainter &painter) const {
    const std::vector<std::uint32_t> &cells = selection.getCells();
    if (cells.empty() || !style.isShown(DisplayLayer::Selection)) {
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, cells.size() <= thinOutlineCells);
    const QPointF lo = view.getScreenToWorld({0.0, static_cast<double>(view.height())});
    const QPointF hi = view.getScreenToWorld({static_cast<double>(view.width()), 0.0});
    std::vector<QRectF> outlines;
    outlines.reserve(std::min(cells.size(), outlinedMacroLimit));
    for (const std::uint32_t cell : cells) {
        const BBox box = frame.getCellBounds(cell);
        if (box.lowerLeft.x > hi.x() || box.upperRight.x < lo.x() || box.lowerLeft.y > hi.y() ||
            box.upperRight.y < lo.y()) {
            continue;
        }
        QRectF outline = getRectOf(box);
        if (outline.width() < pinMarkPx || outline.height() < pinMarkPx) {
            outline = QRectF(outline.center() - QPointF(pinMarkPx, pinMarkPx),
                             QSizeF(2 * pinMarkPx, 2 * pinMarkPx));
        }
        outlines.push_back(outline);
    }
    painter.setPen(
        QPen(style[DisplayLayer::Selection].color, cells.size() <= thinOutlineCells ? 2.0 : 1.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRects(outlines.data(), static_cast<int>(outlines.size()));
}

void LayoutView::Impl::paintRubberBand(QPainter &painter) const {
    if (!rubberBand) {
        return;
    }
    const QColor color = style[DisplayLayer::Selection].color;
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(color, 1.0, Qt::DashLine));
    painter.setBrush(withAlpha(color, boxFillAlpha));
    painter.drawRect(rubberBand->normalized());
}

void LayoutView::Impl::refreshOverlay() const {
    if (layer != nullptr) {
        layer->update();
    } else {
        view.update();
    }
}

std::optional<std::uint32_t> LayoutView::Impl::pick(QPointF screen) const {
    if (!frame.isDrawable()) {
        return std::nullopt;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const QPointF at = view.getScreenToWorld(screen);
    const double slack = pickSlackPx / scale;
    const auto pickLayer = [](CellKind kind) {
        switch (kind) {
            case CellKind::Pad:
                return 0;
            case CellKind::Movable:
                return 1;
            case CellKind::MovableMacro:
                return 2;
            case CellKind::Macro:
                return 3;
        }
        return 3;
    };
    const auto pickable = [this](CellKind kind) {
        return style.isShown(getLayerOf(kind));
    };
    std::optional<std::uint32_t> best;
    int bestLayer = 4;
    double bestDistance = 0.0;
    for (std::uint32_t v = 0; v < frame.x.size(); ++v) {
        const double x0 = frame.x[v];
        const double y0 = frame.y[v];
        const double x1 = x0 + geometry.width[v];
        const double y1 = y0 + geometry.height[v];
        if (at.x() < x0 - slack || at.x() > x1 + slack || at.y() < y0 - slack ||
            at.y() > y1 + slack || !pickable(geometry.kind[v])) {
            continue;
        }
        const int cellLayer = pickLayer(geometry.kind[v]);
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
    if (!frame.isDrawable()) {
        return;
    }
    const LayoutGeometry &geometry = *frame.geometry;
    const auto &die = geometry.die;
    const QColor movableFill = style[DisplayLayer::MovableCells].color;
    const QColor macroFill = style[DisplayLayer::Macros].color;

    QPainter painter(&cache);
    const QPointF lo = view.getScreenToWorld({0.0, static_cast<double>(view.height())});
    const QPointF hi = view.getScreenToWorld({static_cast<double>(view.width()), 0.0});

    if (style.isShown(DisplayLayer::Rows) && !geometry.rows.empty() &&
        geometry.rows.front().height * scale >= visibleRow) {
        painter.setPen(QPen(style[DisplayLayer::Rows].color, 0));
        for (const LayoutGeometry::Row &row : geometry.rows) {
            if (row.y > hi.y() || row.y + row.height < lo.y()) {
                continue;
            }
            const QRectF r = getRectOf(row.xlo, row.y, row.xhi - row.xlo, row.height);
            painter.drawLine(r.bottomLeft(), r.bottomRight());
        }
    }

    const int pw = cache.width();
    const int ph = cache.height();
    const double pixelArea = scale * scale * dpr * dpr;
    std::vector<float> density;
    std::vector<QRectF> movable;
    std::vector<QRectF> macros;
    std::vector<QRectF> blocks;
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
            QRectF r = getRectOf(x, y, w, h);
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
            const QPointF s = view.getWorldToScreen({x + w / 2.0, y + h / 2.0}) * dpr;
            const int ix = static_cast<int>(s.x());
            const int iy = static_cast<int>(s.y());
            if (ix >= 0 && iy >= 0 && ix < pw && iy < ph) {
                density[static_cast<std::size_t>(iy) * static_cast<std::size_t>(pw) +
                        static_cast<std::size_t>(ix)] += static_cast<float>(w * h * pixelArea);
            }
            continue;
        }
        (kind == CellKind::Macro          ? macros
         : kind == CellKind::MovableMacro ? blocks
                                          : movable)
            .push_back(getRectOf(x, y, w, h));
    }

    if (style.isShown(DisplayLayer::Macros)) {
        painter.setPen(macros.size() < outlinedMacroLimit ? QPen(macroFill.darker(edgeDarkening), 0)
                                                          : QPen(Qt::NoPen));
        painter.setBrush(macroFill);
        painter.drawRects(macros.data(), static_cast<int>(macros.size()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QBrush(macroFill.darker(edgeDarkening), Qt::BDiagPattern));
        painter.drawRects(macros.data(), static_cast<int>(macros.size()));
    }

    if (style.isShown(DisplayLayer::MovableMacros)) {
        const QColor blockFill = style[DisplayLayer::MovableMacros].color;
        painter.setPen(QPen(blockFill.darker(edgeDarkening), 0));
        painter.setBrush(blockFill);
        painter.drawRects(blocks.data(), static_cast<int>(blocks.size()));
    }

    if (style.isShown(DisplayLayer::MovableCells)) {
        if (!density.empty()) {
            QImage overlay(pw, ph, QImage::Format_ARGB32_Premultiplied);
            overlay.fill(Qt::transparent);
            for (int py = 0; py < ph; ++py) {
                auto *line = reinterpret_cast<QRgb *>(overlay.scanLine(py));
                const float *sums = density.data() + static_cast<std::size_t>(py) * pw;
                for (int px = 0; px < pw; ++px) {
                    if (sums[px] > 0.0F) {
                        const int alpha =
                            static_cast<int>(std::min(1.0F, 0.25F + sums[px]) * 255.0F);
                        line[px] = qPremultiply(qRgba(movableFill.red(), movableFill.green(),
                                                      movableFill.blue(), alpha));
                    }
                }
            }
            overlay.setDevicePixelRatio(dpr);
            painter.drawImage(QPointF(0.0, 0.0), overlay);
        }
        const bool outline = geometry.typicalWidth * scale >= outlinedCell;
        painter.setPen(outline ? QPen(movableFill.darker(edgeDarkening), 0) : QPen(Qt::NoPen));
        painter.setBrush(outline ? withAlpha(movableFill, 170) : movableFill);
        painter.drawRects(movable.data(), static_cast<int>(movable.size()));
    }

    if (style.isShown(DisplayLayer::Pads)) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(style[DisplayLayer::Pads].color);
        painter.drawRects(pads.data(), static_cast<int>(pads.size()));
    }

    if (style.isShown(DisplayLayer::Die)) {
        painter.setPen(QPen(style[DisplayLayer::Die].color, 0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(getRectOf(die.lowerLeft.x, die.lowerLeft.y,
                                   die.upperRight.x - die.lowerLeft.x,
                                   die.upperRight.y - die.lowerLeft.y));
    }
}

LayoutView::LayoutView(QWidget *parent) : QWidget(parent), pImpl(std::make_unique<Impl>(*this)) {
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    if (GlCanvas::isAvailable()) {
        pImpl->gpu = new GlCanvas(this);
        pImpl->gpu->setStyle(pImpl->style);
        pImpl->layer = new OverlayLayer(this, [impl = pImpl.get()](QPainter &painter) {
            impl->paintOverlay(painter);
        });
        pImpl->layer->raise();
    }
    pImpl->selection.subscribe([impl = pImpl.get()] {
        impl->refreshOverlay();
    });
}

LayoutView::~LayoutView() = default;

void LayoutView::setFrame(LayoutFrame frame) {
    const bool refit = !pImpl->frame.isDrawable() || !frame.isDrawable() ||
                       pImpl->frame.geometry->die != frame.geometry->die;
    const bool newGeometry = !frame.isDrawable() || frame.geometry != pImpl->frame.geometry;
    pImpl->frame = std::move(frame);
    if (newGeometry) {
        pImpl->selection.clear();
    }
    pImpl->updateCongestion();
    if (pImpl->gpu != nullptr) {
        pImpl->gpu->setFrame(pImpl->frame);
    }
    if (refit) {
        zoomFit();
    } else {
        pImpl->invalidate();
    }
}

const LayoutFrame &LayoutView::getFrame() const {
    return pImpl->frame;
}

SelectionSet &LayoutView::getSelection() {
    return pImpl->selection;
}

const SelectionSet &LayoutView::getSelection() const {
    return pImpl->selection;
}

void LayoutView::selectAt(QPointF screen, bool additive) {
    const std::optional<std::uint32_t> hit = pImpl->pick(screen);
    if (additive) {
        if (hit) {
            pImpl->selection.toggleCell(*hit);
        }
        return;
    }
    if (hit) {
        pImpl->selection.selectCells({*hit});
    } else {
        pImpl->selection.clear();
    }
}

std::optional<std::uint32_t> LayoutView::getSelectedCell() const {
    return pImpl->selection.focus();
}

const DisplayStyle &LayoutView::getStyle() const {
    return pImpl->style;
}

void LayoutView::setStyle(const DisplayStyle &newStyle) {
    if (newStyle != pImpl->style) {
        pImpl->applyStyle(newStyle, true);
    }
}

void LayoutView::onStyleChanged(std::function<void(const DisplayStyle &)> listener) {
    pImpl->styleListener = std::move(listener);
}

void LayoutView::showCongestion(bool on, std::size_t binsAcross) {
    pImpl->congestionBins = binsAcross;
    DisplayStyle next = pImpl->style;
    next[DisplayLayer::RoutingDemand].visible = on;
    if (next == pImpl->style) {
        pImpl->updateCongestion();
        pImpl->refreshOverlay();
        return;
    }
    pImpl->applyStyle(next, true);
}

bool LayoutView::isShowingCongestion() const {
    return pImpl->style.isShown(DisplayLayer::RoutingDemand);
}

void LayoutView::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    QAction *toggle = menu.addAction(QStringLiteral("Routing Demand"));
    toggle->setCheckable(true);
    toggle->setChecked(isShowingCongestion());
    connect(toggle, &QAction::toggled, this, [this](bool on) {
        showCongestion(on, pImpl->congestionBins);
    });
    QMenu *grid = menu.addMenu(QStringLiteral("Demand Grid"));
    auto *choices = new QActionGroup(grid);
    for (const std::size_t bins : congestionGrids) {
        QAction *choice = grid->addAction(QStringLiteral("%1 bins across").arg(bins));
        choice->setCheckable(true);
        choice->setChecked(bins == pImpl->congestionBins);
        choices->addAction(choice);
        connect(choice, &QAction::triggered, this, [this, bins] {
            showCongestion(true, bins);
        });
    }
    menu.addSeparator();
    QAction *focusSelection = menu.addAction(QStringLiteral("Zoom to Selection"));
    focusSelection->setEnabled(!pImpl->selection.empty());
    connect(focusSelection, &QAction::triggered, this, [this] {
        zoomToSelection();
    });
    QAction *connectivity = menu.addAction(QStringLiteral("Show Connectivity"));
    connectivity->setEnabled(!pImpl->selection.getCells().empty());
    connect(connectivity, &QAction::triggered, this, [this] {
        showConnectivity();
    });
    QAction *hideNets = menu.addAction(QStringLiteral("Hide Connectivity"));
    hideNets->setEnabled(!pImpl->selection.getNets().empty());
    connect(hideNets, &QAction::triggered, this, [this] {
        hideConnectivity();
    });
    QAction *clear = menu.addAction(QStringLiteral("Select None"));
    clear->setEnabled(!pImpl->selection.empty());
    connect(clear, &QAction::triggered, this, [this] {
        pImpl->selection.clear();
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("Zoom Fit"), this, &LayoutView::zoomFit);
    menu.exec(event->globalPos());
}

std::size_t LayoutView::showConnectivity() {
    if (!pImpl->frame.isDrawable()) {
        return 0;
    }
    const LayoutGeometry &geometry = *pImpl->frame.geometry;
    std::vector<std::uint32_t> nets;
    for (const std::uint32_t cell : pImpl->selection.getCells()) {
        const auto first = geometry.cellNet.begin() + geometry.cellNetStart[cell];
        const auto last = geometry.cellNet.begin() + geometry.cellNetStart[cell + 1];
        nets.insert(nets.end(), first, last);
    }
    pImpl->selection.markNets(std::move(nets));
    return pImpl->selection.getNets().size();
}

void LayoutView::hideConnectivity() {
    pImpl->selection.markNets({});
}

void LayoutView::zoomFit() {
    if (!pImpl->frame.isDrawable()) {
        pImpl->scale = 1.0;
        pImpl->center = {};
        pImpl->invalidate();
        return;
    }
    const auto &die = pImpl->frame.geometry->die;
    zoomTo(die);
}

void LayoutView::zoomTo(const BBox &area) {
    const double floor =
        pImpl->frame.isDrawable() ? minZoomCells * pImpl->frame.geometry->typicalWidth : 1e-9;
    const double w = std::max(area.upperRight.x - area.lowerLeft.x, floor);
    const double h = std::max(area.upperRight.y - area.lowerLeft.y, floor);
    pImpl->scale = std::min(width() / w, height() / h) * fitMargin;
    if (!(pImpl->scale > 0.0) || !std::isfinite(pImpl->scale)) {
        pImpl->scale = 1.0;
    }
    pImpl->center = {(area.lowerLeft.x + area.upperRight.x) / 2.0,
                     (area.lowerLeft.y + area.upperRight.y) / 2.0};
    pImpl->invalidate();
}

bool LayoutView::zoomToSelection() {
    if (!pImpl->frame.isDrawable()) {
        return false;
    }
    std::optional<BBox> area = pImpl->frame.getCellsBounds(pImpl->selection.getCells());
    if (const std::optional<BBox> nets = pImpl->frame.getNetsBounds(pImpl->selection.getNets())) {
        if (area) {
            area->extend(*nets);
        } else {
            area = nets;
        }
    }
    if (!area) {
        return false;
    }
    zoomTo(*area);
    return true;
}

void LayoutView::zoomBy(double factor, QPointF anchor) {
    const QPointF before = getScreenToWorld(anchor);
    pImpl->scale = std::clamp(pImpl->scale * factor, 1e-9, 1e9);
    pImpl->center += before - getScreenToWorld(anchor);
    pImpl->invalidate();
}

void LayoutView::onCursorMoved(std::function<void(std::optional<QPointF>)> listener) {
    pImpl->cursorListener = std::move(listener);
}

QImage LayoutView::capture() {
    return grab().toImage();
}

QPointF LayoutView::getWorldToScreen(QPointF world) const {
    return {(world.x() - pImpl->center.x()) * pImpl->scale + width() / 2.0,
            height() / 2.0 - (world.y() - pImpl->center.y()) * pImpl->scale};
}

QPointF LayoutView::getScreenToWorld(QPointF screen) const {
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
    pImpl->paintOverlay(painter);
    if (!pImpl->frame.isDrawable()) {
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
    if (pImpl->frame.isDrawable() && pImpl->cache.isNull() && pImpl->gpu == nullptr) {
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
    if (event->button() == Qt::LeftButton && event->modifiers().testFlag(Qt::ShiftModifier)) {
        pImpl->rubberBand = QRectF(event->position(), event->position());
        return;
    }
    if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        pImpl->pressAt = event->position();
        pImpl->lastMouse = event->position();
        pImpl->panning = event->button() == Qt::MiddleButton;
    }
}

void LayoutView::mouseMoveEvent(QMouseEvent *event) {
    if (pImpl->cursorListener && pImpl->frame.isDrawable()) {
        pImpl->cursorListener(getScreenToWorld(event->position()));
    }
    if (pImpl->rubberBand) {
        pImpl->rubberBand->setBottomRight(event->position());
        pImpl->refreshOverlay();
        return;
    }
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
    if (pImpl->rubberBand) {
        const QRectF band = pImpl->rubberBand->normalized();
        pImpl->rubberBand.reset();
        pImpl->refreshOverlay();
        if (band.width() > dragThreshold && band.height() > dragThreshold) {
            const QPointF lo = getScreenToWorld(band.bottomLeft());
            const QPointF hi = getScreenToWorld(band.topRight());
            zoomTo(BBox(Point(lo.x(), lo.y()), Point(hi.x(), hi.y())));
        }
        return;
    }
    if (pImpl->pressAt && !pImpl->panning && event->button() == Qt::LeftButton) {
        selectAt(event->position(), event->modifiers().testFlag(Qt::ControlModifier));
    }
    pImpl->pressAt.reset();
    pImpl->panning = false;
    unsetCursor();
}

void LayoutView::leaveEvent(QEvent *) {
    if (pImpl->cursorListener) {
        pImpl->cursorListener(std::nullopt);
    }
}

void LayoutView::keyPressEvent(QKeyEvent *event) {
    const QPointF middle(width() / 2.0, height() / 2.0);
    switch (event->key()) {
        case Qt::Key_F:
            zoomFit();
            break;
        case Qt::Key_Z:
            zoomToSelection();
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
