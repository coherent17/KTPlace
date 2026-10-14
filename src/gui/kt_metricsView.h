// @file kt_metricsView.h
// The Metrics panel: the wirelength and the overflow of a running placement,
// one point per outer iteration, drawn as it arrives.
//
// HPWL is on a log scale on the left (it falls by an order of magnitude or
// more over a run), the scaled overflow linear on the right. SimPL reports two
// wirelengths per global iteration, the legalized upper bound and the lower
// bound, and the gap between them closing is what convergence looks like; both
// are drawn. The warm-up iterations are shaded, because their wirelength is of
// an overlapping placement and not comparable with what follows.
//
// Plain QPainter: the GUI links Qt Widgets only.

#pragma once

#include "util/kt_progress.h"

#include <QString>
#include <QWidget>
#include <cstddef>
#include <string>
#include <vector>

namespace ktplace {

class MetricsView : public QWidget {
public:
    /// One plotted iteration.
    struct Point {
        std::string phase;
        std::size_t iter = 0;
        double hpwl = 0.0;
        double hpwlLower = 0.0;
        double overflow = -1.0;  ///< negative: not measured
    };

    explicit MetricsView(QWidget *parent = nullptr);

    /// Add @p sample's numbers (its positions are ignored). A sample from a new
    /// run starts a new plot.
    void addSample(const ProgressSample &sample);
    void clear();

    [[nodiscard]] const std::vector<Point> &points() const {
        return points_;
    }
    /// "simpl run 3", or empty before the first sample.
    [[nodiscard]] const QString &title() const {
        return title_;
    }

    [[nodiscard]] QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::vector<Point> points_;
    std::size_t run_ = 0;
    QString title_;
};

}  // namespace ktplace
