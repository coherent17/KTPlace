#include "gui/kt_detailsPanel.h"

#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace ktplace::gui {

namespace {

constexpr int netIdRole = Qt::UserRole + 1;

QString kindName(CellKind kind) {
    switch (kind) {
        case CellKind::Movable:
            return QStringLiteral("Movable cell");
        case CellKind::Macro:
            return QStringLiteral("Fixed macro");
        case CellKind::Pad:
            return QStringLiteral("I/O pad");
    }
    return {};
}

QString number(double value) {
    return QString::number(value, 'f', 1);
}

QString boundsText(const Bounds &box) {
    return QStringLiteral("(%1, %2) to (%3, %4)")
        .arg(number(box.xlo), number(box.ylo), number(box.xhi), number(box.yhi));
}

class NumericItem : public QTableWidgetItem {
public:
    NumericItem(double value, const QString &text) : QTableWidgetItem(text), sortKey(value) {}

    bool operator<(const QTableWidgetItem &other) const override {
        const auto *numeric = dynamic_cast<const NumericItem *>(&other);
        return numeric != nullptr ? sortKey < numeric->sortKey : QTableWidgetItem::operator<(other);
    }

private:
    double sortKey;
};

}  // namespace

class DetailsPanel::Impl {
public:
    QLabel *heading = nullptr;
    QTableWidget *properties = nullptr;
    QLabel *netsHeading = nullptr;
    QTableWidget *nets = nullptr;
    std::function<void(std::uint32_t, bool)> listener;

    void setProperties(std::span<const std::pair<QString, QString>> rows) {
        properties->setRowCount(static_cast<int>(rows.size()));
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            const auto &[key, value] = rows[static_cast<std::size_t>(row)];
            properties->setItem(row, 0, new QTableWidgetItem(key));
            properties->setItem(row, 1, new QTableWidgetItem(value));
        }
    }

    void setNets(const LayoutFrame &frame, std::span<const std::uint32_t> ids) {
        const LayoutGeometry &geometry = *frame.geometry;
        nets->setSortingEnabled(false);
        nets->setRowCount(static_cast<int>(ids.size()));
        for (int row = 0; row < static_cast<int>(ids.size()); ++row) {
            const std::uint32_t net = ids[static_cast<std::size_t>(row)];
            const std::uint32_t pins = geometry.netPinStart[net + 1] - geometry.netPinStart[net];
            const std::optional<Bounds> box = frame.netBounds(net);
            const double hpwl = box ? box->halfPerimeter() : 0.0;
            auto *name = new QTableWidgetItem(QString::fromStdString(geometry.netName[net]));
            name->setData(netIdRole, net);
            nets->setItem(row, 0, name);
            nets->setItem(row, 1, new NumericItem(pins, QString::number(pins)));
            nets->setItem(row, 2, new NumericItem(hpwl, number(hpwl)));
        }
        nets->setSortingEnabled(true);
        const bool any = !ids.empty();
        nets->setVisible(any);
        netsHeading->setVisible(any);
        netsHeading->setText(QStringLiteral("Nets (%1)").arg(ids.size()));
    }

    void describeCell(const LayoutFrame &frame, std::uint32_t cell) {
        const LayoutGeometry &geometry = *frame.geometry;
        heading->setText(QString::fromStdString(geometry.name[cell]));
        const std::int32_t fence = geometry.fenceOf[cell];
        const std::uint32_t firstNet = geometry.cellNetStart[cell];
        const std::uint32_t lastNet = geometry.cellNetStart[cell + 1];
        std::size_t pins = 0;
        for (std::uint32_t k = firstNet; k < lastNet; ++k) {
            const std::uint32_t net = geometry.cellNet[k];
            for (std::uint32_t pin = geometry.netPinStart[net]; pin < geometry.netPinStart[net + 1];
                 ++pin) {
                pins += geometry.pinCell[pin] == cell ? 1 : 0;
            }
        }
        const std::array<std::pair<QString, QString>, 7> rows{{
            {QStringLiteral("Type"), kindName(geometry.kind[cell])},
            {QStringLiteral("Size"), QStringLiteral("%1 x %2").arg(number(geometry.width[cell]),
                                                                   number(geometry.height[cell]))},
            {QStringLiteral("Location"),
             QStringLiteral("(%1, %2)").arg(number(frame.x[cell]), number(frame.y[cell]))},
            {QStringLiteral("Area"),
             number(static_cast<double>(geometry.width[cell]) * geometry.height[cell])},
            {QStringLiteral("Fence"),
             fence >= 0 && static_cast<std::size_t>(fence) < geometry.fences.size()
                 ? QString::fromStdString(geometry.fences[fence].name)
                 : QStringLiteral("none")},
            {QStringLiteral("Pins"), QString::number(pins)},
            {QStringLiteral("Index"), QString::number(cell)},
        }};
        setProperties(rows);
        setNets(frame, std::span(geometry.cellNet).subspan(firstNet, lastNet - firstNet));
    }

    void describeCells(const LayoutFrame &frame, std::span<const std::uint32_t> cells) {
        const LayoutGeometry &geometry = *frame.geometry;
        std::array<std::size_t, 3> counts{};
        double area = 0.0;
        for (const std::uint32_t cell : cells) {
            ++counts[static_cast<std::size_t>(geometry.kind[cell])];
            area += static_cast<double>(geometry.width[cell]) * geometry.height[cell];
        }
        heading->setText(QStringLiteral("%1 cells selected").arg(cells.size()));
        const std::optional<Bounds> box = frame.cellsBounds(cells);
        const std::array<std::pair<QString, QString>, 5> rows{{
            {kindName(CellKind::Movable), QString::number(counts[0])},
            {kindName(CellKind::Macro), QString::number(counts[1])},
            {kindName(CellKind::Pad), QString::number(counts[2])},
            {QStringLiteral("Total area"), number(area)},
            {QStringLiteral("Bounds"), box ? boundsText(*box) : QString()},
        }};
        setProperties(rows);
        setNets(frame, {});
    }

    void describeNets(const LayoutFrame &frame, std::span<const std::uint32_t> ids) {
        const LayoutGeometry &geometry = *frame.geometry;
        heading->setText(ids.size() == 1 ? QString::fromStdString(geometry.netName[ids.front()])
                                         : QStringLiteral("%1 nets marked").arg(ids.size()));
        std::size_t pins = 0;
        double hpwl = 0.0;
        for (const std::uint32_t net : ids) {
            pins += geometry.netPinStart[net + 1] - geometry.netPinStart[net];
            if (const std::optional<Bounds> box = frame.netBounds(net)) {
                hpwl += box->halfPerimeter();
            }
        }
        const std::optional<Bounds> box = frame.netsBounds(ids);
        const std::array<std::pair<QString, QString>, 3> rows{{
            {QStringLiteral("Pins"), QString::number(pins)},
            {QStringLiteral("HPWL"), number(hpwl)},
            {QStringLiteral("Bounds"), box ? boundsText(*box) : QString()},
        }};
        setProperties(rows);
        setNets(frame, ids);
    }

    void describeNothing() {
        heading->setText(QStringLiteral("Nothing selected"));
        const std::array<std::pair<QString, QString>, 3> rows{{
            {QStringLiteral("Click"), QStringLiteral("select a cell")},
            {QStringLiteral("Ctrl+click"), QStringLiteral("add or remove a cell")},
            {QStringLiteral("Ctrl+F"), QStringLiteral("locate cells or nets by name")},
        }};
        setProperties(rows);
        nets->setRowCount(0);
        nets->setVisible(false);
        netsHeading->setVisible(false);
    }
};

DetailsPanel::DetailsPanel(QWidget *parent) : QWidget(parent), pImpl(std::make_unique<Impl>()) {
    pImpl->heading = new QLabel(this);
    QFont bold = pImpl->heading->font();
    bold.setBold(true);
    bold.setPointSizeF(bold.pointSizeF() * 1.15);
    pImpl->heading->setFont(bold);
    pImpl->heading->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pImpl->heading->setWordWrap(true);

    pImpl->properties = new QTableWidget(0, 2, this);
    pImpl->properties->horizontalHeader()->hide();
    pImpl->properties->verticalHeader()->hide();
    pImpl->properties->horizontalHeader()->setStretchLastSection(true);
    pImpl->properties->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pImpl->properties->setSelectionMode(QAbstractItemView::NoSelection);
    pImpl->properties->setShowGrid(false);
    pImpl->properties->setAlternatingRowColors(true);
    pImpl->properties->setColumnWidth(0, 110);

    pImpl->netsHeading = new QLabel(this);
    pImpl->nets = new QTableWidget(0, 3, this);
    pImpl->nets->setHorizontalHeaderLabels(
        {QStringLiteral("Net"), QStringLiteral("Pins"), QStringLiteral("HPWL")});
    pImpl->nets->verticalHeader()->hide();
    pImpl->nets->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    pImpl->nets->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    pImpl->nets->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    pImpl->nets->setEditTriggers(QAbstractItemView::NoEditTriggers);
    pImpl->nets->setSelectionBehavior(QAbstractItemView::SelectRows);
    pImpl->nets->setSelectionMode(QAbstractItemView::SingleSelection);
    pImpl->nets->setAlternatingRowColors(true);
    pImpl->nets->setToolTip(QStringLiteral("Click to mark the net, double-click to zoom to it"));
    const auto chosen = [impl = pImpl.get()](QTableWidgetItem *item, bool zoom) {
        const QTableWidgetItem *name = impl->nets->item(item->row(), 0);
        if (impl->listener && name != nullptr) {
            impl->listener(name->data(netIdRole).toUInt(), zoom);
        }
    };
    connect(pImpl->nets, &QTableWidget::itemClicked, this, [chosen](QTableWidgetItem *item) {
        chosen(item, false);
    });
    connect(pImpl->nets, &QTableWidget::itemDoubleClicked, this, [chosen](QTableWidgetItem *item) {
        chosen(item, true);
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(pImpl->heading);
    layout->addWidget(pImpl->properties, 2);
    layout->addWidget(pImpl->netsHeading);
    layout->addWidget(pImpl->nets, 3);
    pImpl->describeNothing();
}

DetailsPanel::~DetailsPanel() = default;

void DetailsPanel::describe(const LayoutFrame &frame, const SelectionSet &selection) {
    if (!frame.drawable()) {
        pImpl->describeNothing();
        return;
    }
    const std::vector<std::uint32_t> &cells = selection.cells();
    if (cells.size() == 1) {
        pImpl->describeCell(frame, cells.front());
    } else if (!cells.empty()) {
        pImpl->describeCells(frame, cells);
    } else if (!selection.nets().empty()) {
        pImpl->describeNets(frame, selection.nets());
    } else {
        pImpl->describeNothing();
    }
}

void DetailsPanel::onNetChosen(std::function<void(std::uint32_t, bool)> listener) {
    pImpl->listener = std::move(listener);
}

QString DetailsPanel::title() const {
    return pImpl->heading->text();
}

int DetailsPanel::netRows() const {
    return pImpl->nets->rowCount();
}

}  // namespace ktplace::gui
