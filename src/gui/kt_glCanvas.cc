#include "gui/kt_glCanvas.h"

#include "util/kt_log.h"

#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

namespace ktplace::gui {

namespace {

enum Kind : int {
    Macro = 0,
    Movable = 1,
    Pad = 2,
    DieFill = 3,
    Row = 4,
    DieEdge = 5,
    MovableMacro = 6
};
constexpr int kinds = 7;

struct CellSlot {
    CellKind kind;
    Kind shaderKind;
    std::size_t range;
    DisplayLayer layer;
};

constexpr std::size_t dieFillRange = 0;
constexpr std::size_t rowRange = 1;
constexpr std::size_t movableRange = 4;
constexpr std::size_t dieEdgeRange = 6;
constexpr std::array<CellSlot, 4> cellSlots{{
    {CellKind::Macro, Macro, 2, DisplayLayer::Macros},
    {CellKind::MovableMacro, MovableMacro, 3, DisplayLayer::MovableMacros},
    {CellKind::Movable, Movable, movableRange, DisplayLayer::MovableCells},
    {CellKind::Pad, Pad, 5, DisplayLayer::Pads},
}};
constexpr double firstTweenMs = 120.0;
constexpr double shortestTweenMs = 60.0;
constexpr double longestTweenMs = 1000.0;
constexpr double intervalSmoothing = 0.3;
constexpr double visibleRowPx = 5.0;
constexpr double densityCellPx = 1.5;
constexpr float movableMinAlpha = 0.35F;
constexpr int edgeDarkening = 140;
constexpr QRgb canvasBackground = 0x101418;
constexpr double outlinedCellPx = 5.0;

const char *vertexShader = R"(#version 330 core
layout(location = 0) in vec2 corner;
layout(location = 1) in vec2 prevPos;
layout(location = 2) in vec2 nextPos;
layout(location = 3) in vec3 shape;
uniform vec2 center;
uniform float scale;
uniform vec2 viewport;
uniform float t;
uniform float dpr;
out vec2 local;
out vec2 drawn;
out float coverage;
flat out int kind;
void main() {
    kind = int(shape.z + 0.5);
    vec2 pos = mix(prevPos, nextPos, t);
    vec2 sizePx = shape.xy * scale;
    float minPx = kind == 2 ? 3.0 * dpr : 1.0;
    vec2 d = max(sizePx, vec2(minPx));
    vec2 c = (pos + shape.xy * 0.5 - center) * scale;
    vec2 p = c + (corner - 0.5) * d;
    gl_Position = vec4(p / (viewport * 0.5), 0.0, 1.0);
    local = corner * d;
    drawn = d;
    coverage = clamp((sizePx.x * sizePx.y) / (d.x * d.y), 0.0, 1.0);
}
)";

const char *fragmentShader = R"(#version 330 core
in vec2 local;
in vec2 drawn;
in float coverage;
flat in int kind;
uniform vec4 fill[7];
uniform vec4 edge[7];
uniform float edgePx;
uniform float outlineCells;
out vec4 color;
void main() {
    vec4 f = fill[kind];
    float e = min(min(local.x, drawn.x - local.x), min(local.y, drawn.y - local.y));
    float smallest = min(drawn.x, drawn.y);
    if (kind == 0 && mod(gl_FragCoord.x + gl_FragCoord.y, 8.0) < 2.5) {
        f.rgb *= 0.72;
    }
    bool outline = kind == 5 || ((kind == 0 || kind == 2 || kind == 6) && smallest > 4.0 * edgePx) ||
                   (kind == 1 && outlineCells > 0.5 && smallest > 3.0 * edgePx);
    if (outline && e < edgePx) {
        f = edge[kind];
    }
    if (kind == 1) {
        f.a *= max(coverage, 0.35);
    }
    if (f.a <= 0.0) {
        discard;
    }
    color = f;
}
)";

const char *densityVertexShader = R"(#version 330 core
layout(location = 0) in vec2 corner;
out vec2 uv;
void main() {
    uv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char *densityFragmentShader = R"(#version 330 core
in vec2 uv;
uniform sampler2D density;
out vec4 color;
void main() {
    color = texture(density, uv);
}
)";

bool isSoftwareRenderer(std::string_view renderer) {
    return renderer.contains("llvmpipe") || renderer.contains("softpipe") ||
           renderer.contains("SwiftShader");
}

QSurfaceFormat baseFormat() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setAlphaBufferSize(0);
    return format;
}

struct GlSupport {
    bool usable = false;
    bool software = false;
};

GlSupport probeSupport() {
    QOpenGLContext context;
    context.setFormat(baseFormat());
    if (!context.create()) {
        return {};
    }
    const QSurfaceFormat got = context.format();
    GlSupport support{.usable = got.majorVersion() > 3 ||
                                (got.majorVersion() == 3 && got.minorVersion() >= 3)};
    QOffscreenSurface surface;
    surface.setFormat(got);
    surface.create();
    if (context.makeCurrent(&surface)) {
        const auto *name =
            reinterpret_cast<const char *>(context.functions()->glGetString(GL_RENDERER));
        support.software = name != nullptr && isSoftwareRenderer(name);
        context.doneCurrent();
    }
    return support;
}

const GlSupport &glSupport() {
    static const GlSupport support = probeSupport();
    return support;
}

QSurfaceFormat canvasFormat() {
    QSurfaceFormat format = baseFormat();
    format.setSamples(glSupport().software ? 0 : 4);
    return format;
}

QVector4D rgba(const QColor &color, float alpha = 1.0F) {
    return {static_cast<float>(color.redF()), static_cast<float>(color.greenF()),
            static_cast<float>(color.blueF()), alpha};
}

}  // namespace

GlCanvas::GlCanvas(QWidget *parent) : QOpenGLWidget(parent) {
    setFormat(canvasFormat());
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    connect(this, &QOpenGLWidget::frameSwapped, this, [this] {
        if (paintedProgress < 1.0F) {
            update();
        }
    });
}

GlCanvas::~GlCanvas() {
    if (!ready) {
        return;
    }
    makeCurrent();
    glDeleteVertexArrays(1, &vao);
    glDeleteVertexArrays(1, &densityVao);
    glDeleteTextures(1, &densityTexture);
    glDeleteBuffers(1, &quad);
    glDeleteBuffers(1, &shapeBuffer);
    glDeleteBuffers(1, &fromBuffer);
    glDeleteBuffers(1, &toBuffer);
    program.reset();
    densityProgram.reset();
    doneCurrent();
}

bool GlCanvas::available() {
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        return false;
    }
    return glSupport().usable;
}

void GlCanvas::setFrame(const LayoutFrame &frame) {
    if (!frame.drawable()) {
        geometry.reset();
        update();
        return;
    }
    const bool freshGeometry = frame.geometry != geometry;
    if (freshGeometry) {
        geometry = frame.geometry;
        geometryDirty = true;
        arrivalClock.invalidate();
        frameIntervalMs = firstTweenMs;
        const LayoutGeometry &g = *geometry;
        const auto extent = [&g](std::uint32_t v) {
            return std::max(g.width[v], g.height[v]);
        };
        order.clear();
        for (const CellSlot &slot : cellSlots) {
            const auto first = static_cast<std::ptrdiff_t>(order.size());
            for (std::uint32_t v = 0; v < g.kind.size(); ++v) {
                if (g.kind[v] == slot.kind) {
                    order.push_back(v);
                }
            }
            if (slot.kind == CellKind::Movable) {
                std::ranges::stable_sort(order.begin() + first, order.end(), std::ranges::greater{},
                                         extent);
                movableExtent.clear();
                for (auto at = order.begin() + first; at != order.end(); ++at) {
                    movableExtent.push_back(extent(*at));
                }
            }
        }
        const auto &die = g.die;
        shape.clear();
        toPositions.clear();
        const auto add = [this](float x, float y, float w, float h, int kind) {
            toPositions.push_back(x);
            toPositions.push_back(y);
            shape.push_back(w);
            shape.push_back(h);
            shape.push_back(static_cast<float>(kind));
        };
        std::size_t at = 0;
        const auto mark = [&](std::size_t range) {
            ranges[range] = {at, shape.size() / 3 - at};
            at = shape.size() / 3;
        };
        add(static_cast<float>(die[0]), static_cast<float>(die[1]),
            static_cast<float>(die[2] - die[0]), static_cast<float>(die[3] - die[1]), DieFill);
        mark(dieFillRange);
        for (const LayoutGeometry::Row &row : g.rows) {
            add(row.xlo, row.y, row.xhi - row.xlo, 0.0F, Row);
        }
        mark(rowRange);
        for (const CellSlot &slot : cellSlots) {
            for (const std::uint32_t v : order) {
                if (g.kind[v] == slot.kind) {
                    add(0.0F, 0.0F, g.width[v], g.height[v], slot.shaderKind);
                }
            }
            mark(slot.range);
        }
        add(static_cast<float>(die[0]), static_cast<float>(die[1]),
            static_cast<float>(die[2] - die[0]), static_cast<float>(die[3] - die[1]), DieEdge);
        mark(dieEdgeRange);
        typicalWidth = g.typicalWidth;
        rowHeight = g.rows.empty() ? 0.0 : g.rows.front().height;
    }
    const std::size_t base = ranges[cellSlots.front().range].first * 2;
    if (!freshGeometry) {
        const float shown = tweenProgress();
        for (std::size_t i = base; i < base + 2 * order.size(); ++i) {
            fromPositions[i] += (toPositions[i] - fromPositions[i]) * shown;
        }
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        toPositions[base + 2 * i] = frame.x[order[i]];
        toPositions[base + 2 * i + 1] = frame.y[order[i]];
    }
    positionsDirty = true;
    if (freshGeometry) {
        fromPositions = toPositions;
        tweenClock.invalidate();
        paintedProgress = 1.0F;
    } else {
        startTween();
    }
    update();
}

void GlCanvas::startTween() {
    if (arrivalClock.isValid()) {
        const auto interval = static_cast<double>(arrivalClock.restart());
        frameIntervalMs += intervalSmoothing * (interval - frameIntervalMs);
    } else {
        arrivalClock.start();
    }
    tweenMs = std::clamp(frameIntervalMs, shortestTweenMs, longestTweenMs);
    tweenClock.start();
    paintedProgress = 0.0F;
}

float GlCanvas::tweenProgress() const {
    if (!tweenClock.isValid() || tweenMs <= 0.0) {
        return 1.0F;
    }
    return std::min(1.0F, static_cast<float>(static_cast<double>(tweenClock.elapsed()) / tweenMs));
}

void GlCanvas::setStyle(const DisplayStyle &newStyle) {
    style = newStyle;
    update();
}

void GlCanvas::setCamera(QPointF newCenter, double newScale) {
    center = newCenter;
    scale = newScale;
    update();
}

void GlCanvas::initializeGL() {
    initializeOpenGLFunctions();
    program = std::make_unique<QOpenGLShaderProgram>();
    program->addShaderFromSourceCode(QOpenGLShader::Vertex, vertexShader);
    program->addShaderFromSourceCode(QOpenGLShader::Fragment, fragmentShader);
    if (!program->link()) {
        return;
    }
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    const std::array<float, 8> corners{0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F};
    glGenBuffers(1, &quad);
    glBindBuffer(GL_ARRAY_BUFFER, quad);
    glBufferData(GL_ARRAY_BUFFER, sizeof(corners), corners.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &shapeBuffer);
    glGenBuffers(1, &fromBuffer);
    glGenBuffers(1, &toBuffer);
    for (unsigned attribute = 1; attribute <= 3; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);

    densityProgram = std::make_unique<QOpenGLShaderProgram>();
    densityProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, densityVertexShader);
    densityProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, densityFragmentShader);
    if (!densityProgram->link()) {
        return;
    }
    glGenVertexArrays(1, &densityVao);
    glBindVertexArray(densityVao);
    glBindBuffer(GL_ARRAY_BUFFER, quad);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glGenTextures(1, &densityTexture);
    glBindTexture(GL_TEXTURE_2D, densityTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    ready = true;

    const auto *name = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const std::string_view renderer = name != nullptr ? name : "";
    if (isSoftwareRenderer(renderer)) {
        ktlog.warning(
            "layout view: OpenGL is rendering on the CPU ({}); large designs animate slowly",
            renderer);
    }
}

void GlCanvas::uploadGeometry() {
    glBindBuffer(GL_ARRAY_BUFFER, shapeBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(shape.size() * sizeof(float)),
                 shape.data(), GL_STATIC_DRAW);
    for (const unsigned buffer : {fromBuffer, toBuffer}) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(toPositions.size() * sizeof(float)),
                     nullptr, GL_DYNAMIC_DRAW);
    }
    geometryDirty = false;
    uploadPositions();
}

void GlCanvas::uploadPositions() {
    const auto bytes = static_cast<GLsizeiptr>(toPositions.size() * sizeof(float));
    glBindBuffer(GL_ARRAY_BUFFER, fromBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, fromPositions.data());
    glBindBuffer(GL_ARRAY_BUFFER, toBuffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, toPositions.data());
    positionsDirty = false;
}

void GlCanvas::drawRange(const Range &range) {
    if (range.count == 0) {
        return;
    }
    const auto offset = [](std::size_t floats) {
        return reinterpret_cast<const void *>(floats * sizeof(float));
    };
    glBindBuffer(GL_ARRAY_BUFFER, fromBuffer);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, offset(range.first * 2));
    glBindBuffer(GL_ARRAY_BUFFER, toBuffer);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, offset(range.first * 2));
    glBindBuffer(GL_ARRAY_BUFFER, shapeBuffer);
    glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 0, offset(range.first * 3));
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, static_cast<GLsizei>(range.count));
}

void GlCanvas::paintGL() {
    glClearColor(0x10 / 255.0F, 0x14 / 255.0F, 0x18 / 255.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!ready || !geometry) {
        return;
    }
    if (geometryDirty) {
        uploadGeometry();
    } else if (positionsDirty) {
        uploadPositions();
    }
    const float t = tweenProgress();
    paintedProgress = t;

    const qreal dpr = devicePixelRatioF();
    const double deviceScale = scale * dpr;
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    program->bind();
    program->setUniformValue(
        "center", QVector2D(static_cast<float>(center.x()), static_cast<float>(center.y())));
    program->setUniformValue("scale", static_cast<float>(deviceScale));
    program->setUniformValue("viewport", QVector2D(static_cast<float>(width() * dpr),
                                                   static_cast<float>(height() * dpr)));
    program->setUniformValue("t", t);
    program->setUniformValue("dpr", static_cast<float>(dpr));
    program->setUniformValue("edgePx", static_cast<float>(dpr));
    program->setUniformValue("outlineCells",
                             typicalWidth * deviceScale >= outlinedCellPx * dpr ? 1.0F : 0.0F);
    const QColor background = QColor::fromRgb(canvasBackground);
    std::array<QVector4D, kinds> fills{};
    std::array<QVector4D, kinds> edges{};
    for (const CellSlot &slot : cellSlots) {
        const QColor color = style[slot.layer].color;
        fills[slot.shaderKind] = rgba(color);
        edges[slot.shaderKind] = rgba(color.darker(edgeDarkening));
    }
    fills[DieFill] = rgba(background);
    edges[DieFill] = rgba(background);
    fills[Row] = rgba(style[DisplayLayer::Rows].color);
    edges[Row] = fills[Row];
    fills[DieEdge] = rgba(background, 0.0F);
    edges[DieEdge] = rgba(style[DisplayLayer::Die].color);
    program->setUniformValueArray("fill", fills.data(), kinds);
    program->setUniformValueArray("edge", edges.data(), kinds);

    glBindVertexArray(vao);
    if (style.shows(DisplayLayer::Die)) {
        drawRange(ranges[dieFillRange]);
    }
    if (style.shows(DisplayLayer::Rows) && rowHeight * deviceScale >= visibleRowPx * dpr) {
        drawRange(ranges[rowRange]);
    }
    for (const CellSlot &slot : cellSlots) {
        if (!style.shows(slot.layer)) {
            continue;
        }
        if (slot.range == movableRange) {
            drawMovable(t, deviceScale);
        } else {
            drawRange(ranges[slot.range]);
        }
    }
    if (style.shows(DisplayLayer::Die)) {
        drawRange(ranges[dieEdgeRange]);
    }
    glBindVertexArray(0);
    program->release();
}

void GlCanvas::drawMovable(float progress, double deviceScale) {
    const Range &movable = ranges[movableRange];
    if (typicalWidth * deviceScale >= densityCellPx) {
        drawRange(movable);
        return;
    }
    const auto visible = static_cast<std::size_t>(
        std::ranges::partition_point(movableExtent,
                                     [deviceScale](float cellExtent) {
                                         return cellExtent * deviceScale >= densityCellPx;
                                     }) -
        movableExtent.begin());
    drawRange({movable.first, visible});
    glBindVertexArray(0);
    drawDensity({movable.first + visible, movable.count - visible}, progress, deviceScale);
    program->bind();
    glBindVertexArray(vao);
}

void GlCanvas::drawDensity(const Range &range, float progress, double deviceScale) {
    const qreal dpr = devicePixelRatioF();
    const auto pixelsWide = static_cast<int>(std::lround(width() * dpr));
    const auto pixelsHigh = static_cast<int>(std::lround(height() * dpr));
    if (pixelsWide <= 0 || pixelsHigh <= 0 || range.count == 0) {
        return;
    }
    const auto columns = static_cast<std::size_t>(pixelsWide);
    const auto pixels = columns * static_cast<std::size_t>(pixelsHigh);
    transmittance.assign(pixels, 1.0F);
    const double halfWide = pixelsWide * 0.5;
    const double halfHigh = pixelsHigh * 0.5;
    const double pixelArea = deviceScale * deviceScale;
    for (std::size_t i = range.first; i < range.first + range.count; ++i) {
        const float x =
            fromPositions[2 * i] + (toPositions[2 * i] - fromPositions[2 * i]) * progress;
        const float y = fromPositions[2 * i + 1] +
                        (toPositions[2 * i + 1] - fromPositions[2 * i + 1]) * progress;
        const double cellWidth = shape[3 * i];
        const double cellHeight = shape[3 * i + 1];
        const double column = (x + cellWidth * 0.5 - center.x()) * deviceScale + halfWide;
        const double row = (y + cellHeight * 0.5 - center.y()) * deviceScale + halfHigh;
        if (column < 0.0 || row < 0.0 || column >= pixelsWide || row >= pixelsHigh) {
            continue;
        }
        const auto alpha = static_cast<float>(
            std::clamp(cellWidth * cellHeight * pixelArea, double{movableMinAlpha}, 1.0));
        transmittance[static_cast<std::size_t>(row) * columns + static_cast<std::size_t>(column)] *=
            1.0F - alpha;
    }

    const QColor fill = style[DisplayLayer::MovableCells].color;
    const auto red = static_cast<std::uint8_t>(fill.red());
    const auto green = static_cast<std::uint8_t>(fill.green());
    const auto blue = static_cast<std::uint8_t>(fill.blue());
    densityPixels.resize(pixels * 4);
    for (std::size_t p = 0; p < pixels; ++p) {
        std::uint8_t *out = &densityPixels[4 * p];
        out[0] = red;
        out[1] = green;
        out[2] = blue;
        out[3] = static_cast<std::uint8_t>(std::lround((1.0F - transmittance[p]) * 255.0F));
    }

    glBindTexture(GL_TEXTURE_2D, densityTexture);
    if (pixelsWide != densityWidth || pixelsHigh != densityHeight) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, pixelsWide, pixelsHigh, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, densityPixels.data());
        densityWidth = pixelsWide;
        densityHeight = pixelsHigh;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pixelsWide, pixelsHigh, GL_RGBA, GL_UNSIGNED_BYTE,
                        densityPixels.data());
    }
    densityProgram->bind();
    glActiveTexture(GL_TEXTURE0);
    densityProgram->setUniformValue("density", 0);
    glBindVertexArray(densityVao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    densityProgram->release();
    glBindTexture(GL_TEXTURE_2D, 0);
}

}  // namespace ktplace::gui
