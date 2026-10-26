#include "gui/kt_detailsPanel.h"

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
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

QString getKindName(CellKind kind) {
    switch (kind) {
        case CellKind::Movable:
            return QStringLiteral("Standard cell");
        case CellKind::Macro:
            return QStringLiteral("Fixed macro");
        case CellKind::MovableMacro:
            return QStringLiteral("Movable macro");
        case CellKind::Pad:
            return QStringLiteral("I/O pad");
    }
    return {};
}

QString getNumber(double value) {
    return QString::number(value, 'f', 1);
}

QString getBoundsText(const BBox &box) {
    return QStringLiteral("(%1, %2) to (%3, %4)")
        .arg(getNumber(box.lowerLeft.x), getNumber(box.lowerLeft.y), getNumber(box.upperRight.x),
             getNumber(box.upperRight.y));
}

class NumericItem : public QTableWidgetItem {
public:
    NumericItem(double value, const QString &text) : QTableWidgetItem(text), sortKey(value) {
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    void setValue(double value, const QString &text) {
        sortKey = value;
        setText(text);
    }

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
    const LayoutGeometry *listedGeometry = nullptr;
    std::vector<std::uint32_t> listedNets;

    [[nodiscard]] std::uint32_t getNetInRow(int row) const {
        return nets->item(row, 0)->data(netIdRole).toUInt();
    }

    void updateNetValues(const LayoutFrame &frame) {
        for (int row = 0; row < nets->rowCount(); ++row) {
            const std::optional<BBox> box = frame.getNetBounds(getNetInRow(row));
            const double hpwl = box ? box->getHalfPerimeter() : 0.0;
            if (auto *item = dynamic_cast<NumericItem *>(nets->item(row, 2))) {
                item->setValue(hpwl, getNumber(hpwl));
            }
        }
    }

    void highlightNets(const std::vector<std::uint32_t> &shown) const {
        const QSignalBlocker blocker(nets->selectionModel());
        nets->clearSelection();
        for (int row = 0; row < nets->rowCount(); ++row) {
            if (std::ranges::binary_search(shown, getNetInRow(row))) {
                nets->selectionModel()->select(
                    nets->model()->index(row, 0),
                    QItemSelectionModel::Select | QItemSelectionModel::Rows);
            }
        }
    }

    void setProperties(std::span<const std::pair<QString, QString>> rows) {
        properties->setRowCount(static_cast<int>(rows.size()));
        for (int row = 0; row < static_cast<int>(rows.size()); ++row) {
            const auto &[key, value] = rows[static_cast<std::size_t>(row)];
            properties->setItem(row, 0, new QTableWidgetItem(key));
            properties->setItem(row, 1, new QTableWidgetItem(value));
        }
    }

    void setNets(const LayoutFrame &frame, std::span<const std::uint32_t> ids) {
        if (listedGeometry == frame.geometry.get() && std::ranges::equal(ids, listedNets)) {
            updateNetValues(frame);
            return;
        }
        listedGeometry = frame.geometry.get();
        listedNets.assign(ids.begin(), ids.end());
        const LayoutGeometry &geometry = *frame.geometry;
        nets->clearSelection();
        nets->setSortingEnabled(false);
        nets->setRowCount(static_cast<int>(ids.size()));
        for (int row = 0; row < static_cast<int>(ids.size()); ++row) {
            const std::uint32_t net = ids[static_cast<std::size_t>(row)];
            const std::uint32_t pins = geometry.netPinStart[net + 1] - geometry.netPinStart[net];
            const std::optional<BBox> box = frame.getNetBounds(net);
            const double hpwl = box ? box->getHalfPerimeter() : 0.0;
            auto *name = new QTableWidgetItem(QString::fromStdString(geometry.netName[net]));
            name->setData(netIdRole, net);
            nets->setItem(row, 0, name);
            nets->setItem(row, 1, new NumericItem(pins, QString::number(pins)));
            nets->setItem(row, 2, new NumericItem(hpwl, getNumber(hpwl)));
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
            {QStringLiteral("Type"), getKindName(geometry.kind[cell])},
            {QStringLiteral("Size"),
             QStringLiteral("%1 x %2").arg(getNumber(geometry.width[cell]),
                                           getNumber(geometry.height[cell]))},
            {QStringLiteral("Location"),
             QStringLiteral("(%1, %2)").arg(getNumber(frame.x[cell]), getNumber(frame.y[cell]))},
            {QStringLiteral("Area"),
             getNumber(static_cast<double>(geometry.width[cell]) * geometry.height[cell])},
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
        std::array<std::size_t, 4> counts{};
        double area = 0.0;
        for (const std::uint32_t cell : cells) {
            ++counts[static_cast<std::size_t>(geometry.kind[cell])];
            area += static_cast<double>(geometry.width[cell]) * geometry.height[cell];
        }
        heading->setText(QStringLiteral("%1 cells selected").arg(cells.size()));
        const std::optional<BBox> box = frame.getCellsBounds(cells);
        const auto count = [&counts](CellKind kind) {
            return QString::number(counts[static_cast<std::size_t>(kind)]);
        };
        const std::array<std::pair<QString, QString>, 6> rows{{
            {getKindName(CellKind::Movable), count(CellKind::Movable)},
            {getKindName(CellKind::MovableMacro), count(CellKind::MovableMacro)},
            {getKindName(CellKind::Macro), count(CellKind::Macro)},
            {getKindName(CellKind::Pad), count(CellKind::Pad)},
            {QStringLiteral("Total area"), getNumber(area)},
            {QStringLiteral("BBox"), box ? getBoundsText(*box) : QString()},
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
            if (const std::optional<BBox> box = frame.getNetBounds(net)) {
                hpwl += box->getHalfPerimeter();
            }
        }
        const std::optional<BBox> box = frame.getNetsBounds(ids);
        const std::array<std::pair<QString, QString>, 3> rows{{
            {QStringLiteral("Pins"), QString::number(pins)},
            {QStringLiteral("HPWL"), getNumber(hpwl)},
            {QStringLiteral("BBox"), box ? getBoundsText(*box) : QString()},
        }};
        setProperties(rows);
        setNets(frame, ids);
    }

    void describeNothing() {
        heading->setText(QStringLiteral("Nothing selected"));
        const std::array<std::pair<QString, QString>, 4> rows{{
            {QStringLiteral("Click"), QStringLiteral("select a cell")},
            {QStringLiteral("Ctrl+click"), QStringLiteral("add or remove a cell")},
            {QStringLiteral("Right-click"), QStringLiteral("show its connectivity")},
            {QStringLiteral("Ctrl+F"), QStringLiteral("locate cells or nets by name")},
        }};
        setProperties(rows);
        listedGeometry = nullptr;
        listedNets.clear();
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
    if (!frame.isDrawable()) {
        pImpl->describeNothing();
        return;
    }
    const std::vector<std::uint32_t> &cells = selection.getCells();
    if (cells.size() == 1) {
        pImpl->describeCell(frame, cells.front());
    } else if (!cells.empty()) {
        pImpl->describeCells(frame, cells);
    } else if (!selection.getNets().empty()) {
        pImpl->describeNets(frame, selection.getNets());
    } else {
        pImpl->describeNothing();
    }
    pImpl->highlightNets(selection.getNets());
}

void DetailsPanel::onNetChosen(std::function<void(std::uint32_t, bool)> listener) {
    pImpl->listener = std::move(listener);
}

QString DetailsPanel::getTitle() const {
    return pImpl->heading->text();
}

int DetailsPanel::getNetRows() const {
    return pImpl->nets->rowCount();
}

}  // namespace ktplace::gui
