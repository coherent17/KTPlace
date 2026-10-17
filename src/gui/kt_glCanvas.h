#pragma once

#include "gui/kt_layout.h"

#include <QElapsedTimer>
#include <QOpenGLExtraFunctions>
#include <QOpenGLWidget>
#include <QPointF>
#include <QTimer>
#include <array>
#include <cstddef>
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
    void drawRange(const Range &range, unsigned buffer);

    std::shared_ptr<const LayoutGeometry> geometry;
    std::vector<std::uint32_t> order;
    std::vector<float> shape;
    std::vector<float> nextPositions;
    std::array<Range, 6> ranges{};
    bool geometryDirty = false;
    bool positionsDirty = false;
    bool tween = false;

    QPointF center;
    double scale = 1.0;
    double typicalWidth = 1.0;
    double rowHeight = 0.0;

    QTimer animation;
    QElapsedTimer sinceFrame;

    std::unique_ptr<QOpenGLShaderProgram> program;
    unsigned vao = 0;
    unsigned quad = 0;
    unsigned shapeBuffer = 0;
    std::array<unsigned, 2> positionBuffers{};
    std::size_t current = 0;
    bool ready = false;
};

}  // namespace ktplace::gui
