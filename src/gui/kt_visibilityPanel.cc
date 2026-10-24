#include "gui/kt_visibilityPanel.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QVBoxLayout>
#include <array>
#include <cstddef>
#include <functional>
#include <string_view>
#include <utility>

namespace ktplace::gui {

namespace {

constexpr int swatchPx = 14;

QString titleOf(DisplayLayer layer) {
    const std::string_view title = DisplayStyle::title(layer);
    return QString::fromUtf8(title.data(), static_cast<qsizetype>(title.size()));
}

QIcon swatch(const QColor &color) {
    QPixmap pixmap(swatchPx, swatchPx);
    pixmap.fill(color);
    return QIcon(pixmap);
}

}  // namespace

class VisibilityPanel::Impl {
public:
    struct Row {
        QCheckBox *shown = nullptr;
        QToolButton *color = nullptr;
    };

    DisplayStyle style;
    std::array<Row, displayLayers.size()> rows{};
    std::function<void(const DisplayStyle &)> listener;

    void sync() {
        for (const DisplayLayer layer : displayLayers) {
            const Row &row = rows[static_cast<std::size_t>(layer)];
            const QSignalBlocker blocker(row.shown);
            row.shown->setChecked(style.shows(layer));
            row.color->setIcon(swatch(style[layer].color));
        }
    }

    void changed() {
        sync();
        if (listener) {
            listener(style);
        }
    }

    void setAll(bool visible) {
        for (const DisplayLayer layer : displayLayers) {
            style[layer].visible = visible;
        }
        changed();
    }
};

VisibilityPanel::VisibilityPanel(QWidget *parent)
    : QWidget(parent), pImpl(std::make_unique<Impl>()) {
    auto *grid = new QGridLayout;
    grid->setColumnStretch(0, 1);
    grid->setVerticalSpacing(2);
    for (const DisplayLayer layer : displayLayers) {
        const auto index = static_cast<int>(layer);
        Impl::Row &row = pImpl->rows[static_cast<std::size_t>(layer)];
        row.shown = new QCheckBox(titleOf(layer), this);
        row.color = new QToolButton(this);
        row.color->setAutoRaise(true);
        row.color->setToolTip(QStringLiteral("Change the colour"));
        grid->addWidget(row.shown, index, 0);
        grid->addWidget(row.color, index, 1);
        connect(row.shown, &QCheckBox::toggled, this, [impl = pImpl.get(), layer](bool on) {
            impl->style[layer].visible = on;
            impl->changed();
        });
        connect(row.color, &QToolButton::clicked, this, [this, layer] {
            const QColor chosen =
                QColorDialog::getColor(pImpl->style[layer].color, this,
                                       QStringLiteral("Colour of %1").arg(titleOf(layer)));
            if (chosen.isValid()) {
                pImpl->style[layer].color = chosen;
                pImpl->changed();
            }
        });
    }

    auto *showAll = new QPushButton(QStringLiteral("Show All"), this);
    auto *hideAll = new QPushButton(QStringLiteral("Hide All"), this);
    auto *reset = new QPushButton(QStringLiteral("Defaults"), this);
    connect(showAll, &QPushButton::clicked, this, [impl = pImpl.get()] {
        impl->setAll(true);
    });
    connect(hideAll, &QPushButton::clicked, this, [impl = pImpl.get()] {
        impl->setAll(false);
    });
    connect(reset, &QPushButton::clicked, this, [impl = pImpl.get()] {
        impl->style = DisplayStyle();
        impl->changed();
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(showAll);
    buttons->addWidget(hideAll);
    buttons->addWidget(reset);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(grid);
    layout->addLayout(buttons);
    layout->addStretch(1);
    pImpl->sync();
}

VisibilityPanel::~VisibilityPanel() = default;

void VisibilityPanel::setStyle(const DisplayStyle &style) {
    pImpl->style = style;
    pImpl->sync();
}

const DisplayStyle &VisibilityPanel::style() const {
    return pImpl->style;
}

void VisibilityPanel::onChanged(std::function<void(const DisplayStyle &)> listener) {
    pImpl->listener = std::move(listener);
}

}  // namespace ktplace::gui
