#include "gui/kt_glCanvas.h"

#include <QGuiApplication>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <algorithm>
#include <cstdint>
#include <utility>

namespace ktplace::gui {

namespace {

enum Kind : int { Macro = 0, Movable = 1, Pad = 2, DieFill = 3, Row = 4, DieEdge = 5 };
constexpr int kinds = 6;
constexpr int tweenMs = 120;
constexpr int frameMs = 16;
constexpr double visibleRowPx = 5.0;
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
uniform vec4 fill[6];
uniform vec4 edge[6];
uniform float edgePx;
uniform float outlineCells;
out vec4 color;
void main() {
    vec4 f = fill[kind];
    float e = min(min(local.x, drawn.x - local.x), min(local.y, drawn.y - local.y));
    float smallest = min(drawn.x, drawn.y);
    bool outline = kind == 5 || ((kind == 0 || kind == 2) && smallest > 4.0 * edgePx) ||
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

QSurfaceFormat canvasFormat() {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(4);
    format.setAlphaBufferSize(0);
    return format;
}

QVector4D rgba(std::uint32_t rgb, float alpha = 1.0F) {
    return {static_cast<float>((rgb >> 16) & 0xff) / 255.0F,
            static_cast<float>((rgb >> 8) & 0xff) / 255.0F, static_cast<float>(rgb & 0xff) / 255.0F,
            alpha};
}

}  // namespace

GlCanvas::GlCanvas(QWidget *parent) : QOpenGLWidget(parent) {
    setFormat(canvasFormat());
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    animation.setInterval(frameMs);
    connect(&animation, &QTimer::timeout, this, [this] {
        update();
        if (sinceFrame.elapsed() >= tweenMs) {
            animation.stop();
        }
    });
}

GlCanvas::~GlCanvas() {
    if (!ready) {
        return;
    }
    makeCurrent();
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &quad);
    glDeleteBuffers(1, &shapeBuffer);
    glDeleteBuffers(2, positionBuffers.data());
    program.reset();
    doneCurrent();
}

bool GlCanvas::available() {
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        return false;
    }
    QOpenGLContext context;
    context.setFormat(canvasFormat());
    if (!context.create()) {
        return false;
    }
    const QSurfaceFormat got = context.format();
    return got.majorVersion() > 3 || (got.majorVersion() == 3 && got.minorVersion() >= 3);
}

void GlCanvas::setFrame(const LayoutFrame &frame) {
    if (!frame.drawable()) {
        geometry.reset();
        update();
        return;
    }
    if (frame.geometry != geometry) {
        geometry = frame.geometry;
        geometryDirty = true;
        tween = false;
        const LayoutGeometry &g = *geometry;
        order.clear();
        for (const CellKind kind : {CellKind::Macro, CellKind::Movable, CellKind::Pad}) {
            for (std::uint32_t v = 0; v < g.kind.size(); ++v) {
                if (g.kind[v] == kind) {
                    order.push_back(v);
                }
            }
        }
        const auto &die = g.die;
        shape.clear();
        nextPositions.clear();
        const auto add = [this](float x, float y, float w, float h, int kind) {
            nextPositions.push_back(x);
            nextPositions.push_back(y);
            shape.push_back(w);
            shape.push_back(h);
            shape.push_back(static_cast<float>(kind));
        };
        std::size_t at = 0;
        const auto mark = [&](int slot) {
            ranges[static_cast<std::size_t>(slot)] = {at, shape.size() / 3 - at};
            at = shape.size() / 3;
        };
        add(static_cast<float>(die[0]), static_cast<float>(die[1]),
            static_cast<float>(die[2] - die[0]), static_cast<float>(die[3] - die[1]), DieFill);
        mark(0);
        for (const LayoutGeometry::Row &row : g.rows) {
            add(row.xlo, row.y, row.xhi - row.xlo, 0.0F, Row);
        }
        mark(1);
        for (const CellKind kind : {CellKind::Macro, CellKind::Movable, CellKind::Pad}) {
            for (const std::uint32_t v : order) {
                if (g.kind[v] == kind) {
                    add(0.0F, 0.0F, g.width[v], g.height[v],
                        kind == CellKind::Macro ? Macro
                                                : (kind == CellKind::Movable ? Movable : Pad));
                }
            }
            mark(kind == CellKind::Macro ? 2 : (kind == CellKind::Movable ? 3 : 4));
        }
        add(static_cast<float>(die[0]), static_cast<float>(die[1]),
            static_cast<float>(die[2] - die[0]), static_cast<float>(die[3] - die[1]), DieEdge);
        mark(5);
        typicalWidth = g.typicalWidth;
        rowHeight = g.rows.empty() ? 0.0 : g.rows.front().height;
    } else {
        tween = true;
    }
    const std::size_t base = ranges[2].first * 2;
    for (std::size_t i = 0; i < order.size(); ++i) {
        nextPositions[base + 2 * i] = frame.x[order[i]];
        nextPositions[base + 2 * i + 1] = frame.y[order[i]];
    }
    positionsDirty = true;
    sinceFrame.start();
    if (tween) {
        animation.start();
    }
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
    glGenBuffers(2, positionBuffers.data());
    for (unsigned attribute = 1; attribute <= 3; ++attribute) {
        glEnableVertexAttribArray(attribute);
        glVertexAttribDivisor(attribute, 1);
    }
    glBindVertexArray(0);
    ready = true;
}

void GlCanvas::uploadGeometry() {
    glBindBuffer(GL_ARRAY_BUFFER, shapeBuffer);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(shape.size() * sizeof(float)),
                 shape.data(), GL_STATIC_DRAW);
    for (const unsigned buffer : positionBuffers) {
        glBindBuffer(GL_ARRAY_BUFFER, buffer);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(nextPositions.size() * sizeof(float)),
                     nextPositions.data(), GL_DYNAMIC_DRAW);
    }
    geometryDirty = false;
    positionsDirty = false;
}

void GlCanvas::drawRange(const Range &range, unsigned previous) {
    if (range.count == 0) {
        return;
    }
    const auto offset = [](std::size_t floats) {
        return reinterpret_cast<const void *>(floats * sizeof(float));
    };
    glBindBuffer(GL_ARRAY_BUFFER, previous);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, offset(range.first * 2));
    glBindBuffer(GL_ARRAY_BUFFER, positionBuffers[current]);
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
        current ^= 1U;
        glBindBuffer(GL_ARRAY_BUFFER, positionBuffers[current]);
        glBufferSubData(GL_ARRAY_BUFFER, 0,
                        static_cast<GLsizeiptr>(nextPositions.size() * sizeof(float)),
                        nextPositions.data());
        positionsDirty = false;
    }
    const unsigned previous = tween ? positionBuffers[current ^ 1U] : positionBuffers[current];
    const float t =
        tween ? std::min(1.0F, static_cast<float>(sinceFrame.elapsed()) / tweenMs) : 1.0F;

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
    const std::array<QVector4D, kinds> fills{rgba(0xd94d4a), rgba(0x4ab5e5), rgba(0xd94d4a),
                                             rgba(0x101418), rgba(0x3b454e), rgba(0x000000, 0.0F)};
    const std::array<QVector4D, kinds> edges{rgba(0x9c3735), rgba(0x3582a5), rgba(0x9c3735),
                                             rgba(0x101418), rgba(0x3b454e), rgba(0x8b97a3)};
    program->setUniformValueArray("fill", fills.data(), kinds);
    program->setUniformValueArray("edge", edges.data(), kinds);

    glBindVertexArray(vao);
    drawRange(ranges[0], previous);
    if (rowHeight * deviceScale >= visibleRowPx * dpr) {
        drawRange(ranges[1], previous);
    }
    for (std::size_t slot = 2; slot < ranges.size(); ++slot) {
        drawRange(ranges[slot], previous);
    }
    glBindVertexArray(0);
    program->release();
}

}  // namespace ktplace::gui
