#pragma once

#include "gui/kt_displayStyle.h"
#include "gui/kt_layout.h"

#include <QElapsedTimer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QPointF>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class QOpenGLShaderProgram;

namespace ktplace::gui {

class GlCanvas : public QOpenGLWidget, protected QOpenGLExtraFunctions {
public:
    explicit GlCanvas(QWidget *parent);
    ~GlCanvas() override;

    GlCanvas(const GlCanvas &) = delete;
    GlCanvas &operator=(const GlCanvas &) = delete;

    void setFrame(const LayoutFrame &frame);
    void setCamera(QPointF center, double scale);
    void setStyle(const DisplayStyle &newStyle);

    [[nodiscard]] static bool available();

protected:
    void initializeGL() override;
    void paintGL() override;

private:
    struct Range {
        std::size_t first = 0;
        std::size_t count = 0;
    };

    void uploadGeometry();
    void uploadPositions();
    void drawRange(const Range &range);
    void drawMovable(float progress, double deviceScale);
    void drawDensity(const Range &range, float progress, double deviceScale);
    void startTween();
    [[nodiscard]] float tweenProgress() const;

    std::shared_ptr<const LayoutGeometry> geometry;
    std::vector<std::uint32_t> order;
    std::vector<float> movableExtent;
    std::vector<float> shape;
    std::vector<float> fromPositions;
    std::vector<float> toPositions;
    std::array<Range, 6> ranges{};
    bool geometryDirty = false;
    bool positionsDirty = false;

    DisplayStyle style;
    QPointF center;
    double scale = 1.0;
    double typicalWidth = 1.0;
    double rowHeight = 0.0;

    QElapsedTimer tweenClock;
    QElapsedTimer arrivalClock;
    double tweenMs = 0.0;
    double frameIntervalMs = 0.0;
    float paintedProgress = 1.0F;

    std::vector<float> transmittance;
    std::vector<std::uint8_t> densityPixels;

    std::unique_ptr<QOpenGLShaderProgram> program;
    std::unique_ptr<QOpenGLShaderProgram> densityProgram;
    unsigned vao = 0;
    unsigned densityVao = 0;
    unsigned densityTexture = 0;
    int densityWidth = 0;
    int densityHeight = 0;
    unsigned quad = 0;
    unsigned shapeBuffer = 0;
    unsigned fromBuffer = 0;
    unsigned toBuffer = 0;
    bool ready = false;
};

}  // namespace ktplace::gui
