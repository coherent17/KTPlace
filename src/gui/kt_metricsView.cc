// @file kt_metricsView.cc
// Implementation of MetricsView.

#include "gui/kt_metricsView.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <algorithm>
#include <cmath>
#include <limits>

namespace ktplace {

namespace {

const QColor kBackground(0x1e, 0x1e, 0x1e);
const QColor kAxis(0x80, 0x80, 0x80);
const QColor kGrid(0x33, 0x33, 0x33);
const QColor kText(0xc8, 0xc8, 0xc8);
const QColor kWarmUp(0x2a, 0x2a, 0x2a);
const QColor kUpper(0x4a, 0x90, 0xd9);
const QColor kLower(0x8c, 0xc0, 0xf2);
const QColor kOverflow(0xe0, 0x8a, 0x2a);

constexpr int kLeft = 74;
constexpr int kRight = 58;
constexpr int kTop = 26;
constexpr int kBottom = 28;

}  // namespace

MetricsView::MetricsView(QWidget *parent) : QWidget(parent) {
    setMinimumSize(240, 140);
    setAttribute(Qt::WA_OpaquePaintEvent);
}

QSize MetricsView::sizeHint() const {
    return {640, 260};
}

void MetricsView::clear() {
    points_.clear();
    run_ = 0;
    title_.clear();
    update();
}

void MetricsView::addSample(const ProgressSample &sample) {
    if (sample.run != run_) {
        points_.clear();
        run_ = sample.run;
        title_ = QStringLiteral("%1 run %2").arg(QString::fromStdString(sample.stage)).arg(run_);
    }
    points_.push_back({sample.phase, sample.iter, sample.hpwl, sample.hpwlLower, sample.overflow});
    update();
}

void MetricsView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    p.setPen(kText);
    if (points_.empty()) {
        p.drawText(rect(), Qt::AlignCenter,
                   QStringLiteral("HPWL and overflow are plotted here while a global "
                                  "placement runs."));
        return;
    }

    const QRectF plot(kLeft, kTop, std::max(width() - kLeft - kRight, 10),
                      std::max(height() - kTop - kBottom, 10));

    // The ranges: log10 of every positive wirelength, and the overflow from 0.
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    double ovfMax = 0.0;
    bool anyOverflow = false;
    for (const Point &pt : points_) {
        for (const double h : {pt.hpwl, pt.hpwlLower}) {
            if (h > 0.0) {
                lo = std::min(lo, std::log10(h));
                hi = std::max(hi, std::log10(h));
            }
        }
        if (pt.overflow >= 0.0) {
            ovfMax = std::max(ovfMax, pt.overflow);
            anyOverflow = true;
        }
    }
    if (lo > hi) {
        lo = 0.0;
        hi = 1.0;
    }
    if (hi - lo < 0.05) {
        lo -= 0.025;
        hi += 0.025;
    }
    if (ovfMax <= 0.0) {
        ovfMax = 1.0;
    }

    const std::size_t n = points_.size();
    const auto xAt = [&](std::size_t i) {
        return n < 2 ? plot.center().x()
                     : plot.left() +
                           plot.width() * static_cast<double>(i) / static_cast<double>(n - 1);
    };
    const auto yLog = [&](double h) {
        return plot.bottom() - plot.height() * (std::log10(h) - lo) / (hi - lo);
    };
    const auto yOvf = [&](double o) {
        return plot.bottom() - plot.height() * o / ovfMax;
    };

    // The warm-up, shaded.
    std::size_t warm = 0;
    while (warm < n && points_[warm].phase == "init") {
        ++warm;
    }
    if (warm > 0) {
        const double right = warm < n ? (xAt(warm - 1) + xAt(warm)) / 2.0 : plot.right();
        p.fillRect(QRectF(QPointF(plot.left(), plot.top()), QPointF(right, plot.bottom())),
                   kWarmUp);
        p.setPen(kAxis);
        p.drawText(QRectF(plot.left() + 4, plot.top() + 2, right - plot.left(), 16),
                   Qt::AlignLeft | Qt::AlignTop, QStringLiteral("warm-up"));
    }

    // Grid and the axis labels: four steps on each scale.
    QFont small = font();
    small.setPointSizeF(std::max(7.0, small.pointSizeF() * 0.85));
    p.setFont(small);
    for (int k = 0; k <= 4; ++k) {
        const double f = k / 4.0;
        const double y = plot.bottom() - plot.height() * f;
        p.setPen(kGrid);
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        p.setPen(kUpper);
        p.drawText(QRectF(0, y - 8, kLeft - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(std::pow(10.0, lo + (hi - lo) * f), 'e', 2));
        if (anyOverflow) {
            p.setPen(kOverflow);
            p.drawText(QRectF(plot.right() + 6, y - 8, kRight - 6, 16),
                       Qt::AlignLeft | Qt::AlignVCenter, QString::number(ovfMax * f, 'f', 3));
        }
    }
    p.setPen(kAxis);
    p.drawRect(plot);
    p.setPen(kText);
    p.drawText(QRectF(plot.left(), plot.bottom() + 4, plot.width(), kBottom - 4),
               Qt::AlignHCenter | Qt::AlignTop,
               QStringLiteral("iteration (%1 so far)").arg(static_cast<qulonglong>(n)));

    // The curves.
    p.setRenderHint(QPainter::Antialiasing);
    // An overflow of 0 is a value (fully spread); a wirelength of 0 is "none".
    const auto curve = [&](auto value, auto yOf, const QPen &pen, bool zeroIsValue) {
        QPainterPath path;
        bool started = false;
        for (std::size_t i = 0; i < n; ++i) {
            const double v = value(points_[i]);
            if (!(v > 0.0) && !(zeroIsValue && v == 0.0)) {
                started = false;
                continue;
            }
            const QPointF at(xAt(i), yOf(v));
            if (started) {
                path.lineTo(at);
            } else {
                path.moveTo(at);
                started = true;
            }
        }
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        if (n == 1) {
            const double v = value(points_[0]);
            if (v > 0.0 || (zeroIsValue && v == 0.0)) {
                p.setBrush(pen.color());
                p.drawEllipse(QPointF(xAt(0), yOf(v)), 2.5, 2.5);
            }
        }
    };
    QPen lowerPen(kLower, 1.2, Qt::DashLine);
    curve(
        [](const Point &pt) {
            return pt.phase == "init" ? 0.0 : pt.hpwlLower;
        },
        yLog, lowerPen, false);
    curve(
        [](const Point &pt) {
            return pt.hpwl;
        },
        yLog, QPen(kUpper, 1.8), false);
    if (anyOverflow) {
        curve(
            [](const Point &pt) {
                return pt.overflow;
            },
            yOvf, QPen(kOverflow, 1.5), true);
    }

    // Title, legend and the latest numbers.
    p.setRenderHint(QPainter::Antialiasing, false);
    const Point &last = points_.back();
    QString latest = QStringLiteral("%1   %2 iter %3   HPWL %4")
                         .arg(title_, QString::fromStdString(last.phase))
                         .arg(static_cast<qulonglong>(last.iter))
                         .arg(last.hpwl, 0, 'e', 4);
    if (last.overflow >= 0.0) {
        latest += QStringLiteral("   overflow %1").arg(last.overflow, 0, 'f', 4);
    }
    p.setPen(kText);
    p.drawText(QRectF(kLeft, 4, plot.width(), kTop - 6), Qt::AlignLeft | Qt::AlignVCenter, latest);
    const auto legend = [&](double x, const QColor &c, Qt::PenStyle style, const QString &text) {
        p.setPen(QPen(c, 2, style));
        p.drawLine(QPointF(x, 14), QPointF(x + 18, 14));
        p.setPen(kText);
        p.drawText(QPointF(x + 22, 18), text);
    };
    const double lx = plot.right() - 250;
    legend(lx, kUpper, Qt::SolidLine, QStringLiteral("HPWL"));
    legend(lx + 70, kLower, Qt::DashLine, QStringLiteral("lower"));
    legend(lx + 140, kOverflow, Qt::SolidLine, QStringLiteral("overflow"));
}

}  // namespace ktplace
