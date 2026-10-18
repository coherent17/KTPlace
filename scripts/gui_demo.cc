#include "gui/kt_layout.h"
#include "gui/kt_layoutView.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_tclConsole.h"
#include "util/kt_log.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QTabBar>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

using namespace ktplace;
using namespace ktplace::gui;

namespace {

constexpr int frameWidth = 1440;
const QColor captionColour(255, 213, 79);
const QColor rightClickColour(236, 72, 153);

class Recorder {
public:
    Recorder(MainWindow &mainWindow, const QString &dir) : window(mainWindow), frameDir(dir) {
        list.setFileName(dir + "/frames.txt");
        list.open(QIODevice::WriteOnly | QIODevice::Text);
        out.setDevice(&list);
        out << "ffconcat version 1.0\n";
    }

    QString caption;
    QPointF pointer{-1, -1};
    std::function<QPointF()> follow;

    void hidePointer() {
        pointer = {-1, -1};
    }

    void pump(int milliseconds) {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < milliseconds) {
            QApplication::processEvents(QEventLoop::AllEvents, 10);
            QThread::msleep(4);
        }
    }

    void snap(double seconds) {
        QApplication::processEvents();
        if (follow) {
            pointer = follow();
        }
        const double scale = static_cast<double>(frameWidth) / window.width();
        QImage frame = window.grab()
                           .toImage()
                           .scaledToWidth(frameWidth, Qt::SmoothTransformation)
                           .convertToFormat(QImage::Format_RGB32);
        frame.setDevicePixelRatio(1.0);
        QPainter painter(&frame);
        painter.setRenderHint(QPainter::Antialiasing);
        drawCaption(painter, scale);
        if (pointer.x() >= 0) {
            if (ring >= 0) {
                drawClick(painter, pointer * scale);
            }
            drawPointer(painter, pointer * scale);
        }
        painter.end();
        const QString name = QStringLiteral("f%1.png").arg(frameCount++, 5, 10, QChar('0'));
        frame.save(frameDir + "/" + name, nullptr, 20);
        out << "file " << name << "\nduration " << QString::number(seconds, 'f', 3) << "\n";
        out.flush();
    }

    void hold(double seconds) {
        pump(80);
        snap(seconds);
    }

    void click(bool right) {
        rightClick = right;
        for (ring = 0; ring < 7; ++ring) {
            snap(0.06);
        }
        snap(0.5);
        ring = -1;
    }

    void moveTo(QPointF target, int steps = 12) {
        const QPointF from =
            pointer.x() < 0 ? QPointF(window.width() * 0.5, window.height() * 0.4) : pointer;
        for (int i = 1; i <= steps; ++i) {
            const double t = static_cast<double>(i) / steps;
            pointer = from + (target - from) * (t * t * (3 - 2 * t));
            snap(0.035);
        }
    }

    [[nodiscard]] QLineEdit *input() const {
        return window.tclConsole()->findChild<QLineEdit *>();
    }

    void key(int code, const QString &text = {}) {
        QKeyEvent press(QEvent::KeyPress, code, Qt::NoModifier, text);
        QApplication::sendEvent(input(), &press);
        QKeyEvent release(QEvent::KeyRelease, code, Qt::NoModifier, text);
        QApplication::sendEvent(input(), &release);
    }

    void type(const QString &command) {
        input()->setFocus();
        for (const QChar letter : command) {
            key(0, QString(letter));
            pump(10);
            snap(0.055);
        }
        hold(0.4);
        key(Qt::Key_Return);
        snap(0.15);
    }

    void waitIdle(int sampleMs, double frameSeconds) {
        while (window.running()) {
            pump(sampleMs);
            snap(frameSeconds);
        }
        pump(150);
    }

    void command(const QString &text, double after, int sampleMs = 300,
                 double frameSeconds = 0.12) {
        type(text);
        waitIdle(sampleMs, frameSeconds);
        hold(after);
    }

private:
    MainWindow &window;
    QString frameDir;
    QFile list;
    QTextStream out;
    int frameCount = 0;
    int ring = -1;
    bool rightClick = false;

    void drawCaption(QPainter &painter, double scale) const {
        if (caption.isEmpty()) {
            return;
        }
        QFont font = painter.font();
        font.setPixelSize(24);
        font.setBold(true);
        painter.setFont(font);
        const LayoutView *view = window.layoutView();
        const QRectF area(view->mapTo(&window, QPoint(0, 0)) * scale,
                          view->size().toSizeF() * scale);
        const QRectF text =
            painter.boundingRect(QRectF(0, 0, frameWidth, 100), Qt::AlignCenter, caption);
        const QRectF box(area.center().x() - text.width() / 2 - 22, area.top() + 14,
                         text.width() + 44, text.height() + 20);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 200));
        painter.drawRoundedRect(box, 10, 10);
        painter.setPen(captionColour);
        painter.drawText(box, Qt::AlignCenter, caption);
    }

    void drawClick(QPainter &painter, QPointF at) const {
        const QColor colour = rightClick ? rightClickColour : captionColour;
        QColor faded = colour;
        faded.setAlpha(255 - ring * 30);
        painter.setPen(QPen(faded, 4));
        painter.setBrush(Qt::NoBrush);
        const double radius = 10 + ring * 6;
        painter.drawEllipse(at, radius, radius);
        QFont font = painter.font();
        font.setPixelSize(18);
        font.setBold(true);
        painter.setFont(font);
        const QRectF tag(at + QPointF(26, 30), QSizeF(120, 30));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 200));
        painter.drawRoundedRect(tag, 8, 8);
        painter.setPen(colour);
        painter.drawText(tag, Qt::AlignCenter,
                         rightClick ? QStringLiteral("Right click") : QStringLiteral("Left click"));
    }

    static void drawPointer(QPainter &painter, QPointF at) {
        QPainterPath arrow;
        arrow.moveTo(at);
        for (const QPointF corner : {QPointF(0, 26), QPointF(7, 20), QPointF(12, 31),
                                     QPointF(17, 29), QPointF(12, 18), QPointF(20, 18)}) {
            arrow.lineTo(at + corner);
        }
        arrow.closeSubpath();
        painter.setPen(QPen(Qt::black, 2));
        painter.setBrush(Qt::white);
        painter.drawPath(arrow);
    }
};

QPointF centreOf(const QWidget *widget, const QWidget *window) {
    return widget->mapTo(window, widget->rect().center());
}

QPointF cellCentre(const LayoutView &view, std::uint32_t cell) {
    const LayoutFrame &frame = view.frame();
    return view.worldToScreen({frame.x[cell] + frame.geometry->width[cell] / 2.0,
                               frame.y[cell] + frame.geometry->height[cell] / 2.0});
}

int fanout(const LayoutGeometry &geometry, std::uint32_t cell) {
    if (geometry.kind[cell] != CellKind::Movable) {
        return -1;
    }
    int nets = 0;
    int pins = 0;
    for (std::uint32_t k = geometry.cellNetStart[cell]; k < geometry.cellNetStart[cell + 1]; ++k) {
        const std::uint32_t net = geometry.cellNet[k];
        const auto degree =
            static_cast<int>(geometry.netPinStart[net + 1] - geometry.netPinStart[net]);
        if (degree <= 40) {
            ++nets;
            pins += degree;
        }
    }
    return nets * 1000 + pins;
}

std::optional<std::uint32_t> selectHighFanoutCell(LayoutView &view) {
    const LayoutGeometry &geometry = *view.frame().geometry;
    std::vector<std::pair<int, std::uint32_t>> ranked;
    for (std::uint32_t cell = 0; cell < geometry.kind.size(); ++cell) {
        if (const int value = fanout(geometry, cell); value > 0) {
            ranked.emplace_back(value, cell);
        }
    }
    std::ranges::sort(ranked, std::greater{});
    ranked.resize(std::min<std::size_t>(ranked.size(), 200));
    const QRectF visible(QPointF(0, 0), view.size().toSizeF());
    for (const auto &[value, cell] : ranked) {
        const QPointF at = cellCentre(view, cell);
        if (!visible.contains(at)) {
            continue;
        }
        view.selectAt(at);
        if (view.selectedCell() == cell) {
            return cell;
        }
    }
    return std::nullopt;
}

QTabBar *findTab(MainWindow &window, const QString &text, int &index) {
    for (QTabBar *bar : window.findChildren<QTabBar *>()) {
        for (int i = 0; i < bar->count(); ++i) {
            if (bar->tabText(i) == text) {
                index = i;
                return bar;
            }
        }
    }
    return nullptr;
}

QToolButton *findButton(MainWindow &window, const QString &prefix) {
    for (QToolButton *button : window.findChildren<QToolButton *>()) {
        if (button->defaultAction() != nullptr &&
            button->defaultAction()->text().startsWith(prefix)) {
            return button;
        }
    }
    return nullptr;
}

void zoomSteps(Recorder &recorder, LayoutView &view, int steps, double factor,
               const std::function<QPointF()> &anchor, double seconds) {
    for (int i = 0; i < steps; ++i) {
        view.zoomBy(factor, anchor());
        recorder.pump(30);
        recorder.snap(seconds);
    }
}

void play(MainWindow &window, Recorder &recorder, const QString &design) {
    LayoutView &view = *window.layoutView();
    const QPointF origin = view.mapTo(&window, QPoint(0, 0));

    recorder.caption = QStringLiteral("KTPlace GUI: ktplace --gui");
    recorder.hold(1.8);

    recorder.caption = QStringLiteral("1. Open Design... and pick a design directory");
    recorder.moveTo(centreOf(findButton(window, QStringLiteral("Open")), &window));
    recorder.hold(1.2);
    recorder.click(false);
    window.openDesign(design);
    recorder.caption = QStringLiteral("%1 loads straight away").arg(QFileInfo(design).fileName());
    recorder.waitIdle(250, 0.15);
    view.zoomFit();
    recorder.hold(2.2);

    int consoleTab = -1;
    QTabBar *tabs = findTab(window, QStringLiteral("Tcl Console"), consoleTab);
    recorder.caption = QStringLiteral("2. The Tcl Console tab drives the same session");
    recorder.moveTo(tabs->mapTo(&window, tabs->tabRect(consoleTab).center()));
    recorder.click(false);
    tabs->setCurrentIndex(consoleTab);
    window.findChild<QDockWidget *>(QStringLiteral("tclConsole"))->raise();
    recorder.hold(1.0);
    recorder.moveTo(centreOf(recorder.input(), &window) -
                    QPointF(recorder.input()->width() * 0.3, 0));
    recorder.hidePointer();

    recorder.caption = QStringLiteral("help lists the ktplace commands");
    recorder.command(QStringLiteral("help"), 3.2);

    recorder.caption = QStringLiteral("3. gplace_design: global placement, streamed live");
    recorder.type(QStringLiteral("gplace_design"));
    recorder.pump(150);
    recorder.snap(0.3);

    recorder.caption = QStringLiteral("Click a high-fanout cell: gplace pulls its nets together");
    if (const std::optional<std::uint32_t> cell = selectHighFanoutCell(view)) {
        recorder.follow = [&view, &origin, id = *cell] {
            return origin + cellCentre(view, id);
        };
        recorder.click(false);
        recorder.follow = nullptr;
    }
    recorder.hidePointer();
    recorder.waitIdle(200, 0.22);
    recorder.hold(1.5);

    recorder.caption = QStringLiteral("report_hpwl returns the wirelength");
    recorder.command(QStringLiteral("report_hpwl"), 1.6);

    const auto selected = [&view] {
        const std::optional<std::uint32_t> cell = view.selectedCell();
        return cell ? cellCentre(view, *cell) : QPointF(view.width() / 2.0, view.height() / 2.0);
    };
    recorder.caption = QStringLiteral("Zoom in to watch the cells shift");
    zoomSteps(recorder, view, 22, 1.15, selected, 0.1);
    recorder.hold(1.2);
    recorder.caption = QStringLiteral("4. legalize_placement snaps cells into rows");
    recorder.command(QStringLiteral("legalize_placement"), 2.0, 120, 0.18);
    recorder.caption = QStringLiteral("detail_place shortens the nets further");
    recorder.command(QStringLiteral("detail_place"), 2.0, 120, 0.18);
    recorder.caption = QStringLiteral("check_placement: 1 means legal");
    recorder.command(QStringLiteral("check_placement"), 1.8);
    zoomSteps(recorder, view, 22, 1.0 / 1.15, selected, 0.06);
    view.zoomFit();
    view.selectAt(QPointF(12, view.height() * 0.5));
    recorder.hold(0.8);

    recorder.caption = QStringLiteral("5. Right-click: the routing-demand map");
    recorder.moveTo(origin + QPointF(view.width() * 0.6, view.height() * 0.45), 24);
    recorder.hold(0.6);
    recorder.click(true);
    view.showCongestion(true);
    recorder.hold(4.0);
    recorder.caption = QStringLiteral("Each net spreads 1/(w*h) over its bounding box");
    recorder.hold(4.0);
    recorder.caption = QStringLiteral("Red bins: where many nets compete for routing");
    recorder.hold(4.0);
    recorder.caption = QStringLiteral("Right-click again to hide it");
    recorder.hold(0.6);
    recorder.click(true);
    view.showCongestion(false);
    recorder.hold(1.5);
    recorder.hidePointer();

    recorder.caption = QStringLiteral("6. Up recalls earlier commands; history persists");
    recorder.input()->setFocus();
    for (int i = 0; i < 3; ++i) {
        recorder.key(Qt::Key_Up);
        recorder.hold(0.6);
    }
    recorder.input()->clear();
    recorder.caption = QStringLiteral("write_placement saves placed.pl");
    recorder.command(QStringLiteral("write_placement"), 3.5);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: gui_demo <work-dir> <design-dir> <frame-dir>\n");
        return 2;
    }
    QApplication app(argc, argv);
    ktlog.throwOnFatal(true);
    MainWindow window({"", argv[1], ""});
    window.showMaximized();
    Recorder recorder(window, QString::fromUtf8(argv[3]));
    const QString design = QString::fromUtf8(argv[2]);
    QTimer::singleShot(1200, [&] {
        play(window, recorder, design);
        std::fflush(nullptr);
        std::_Exit(0);
    });
    return QApplication::exec();
}
