// @file kt_propertyPanel.cc
// Implementation of CellDetails and PropertyPanel.

#include "gui/kt_propertyPanel.h"

#include "session/kt_session.h"

#include <QHeaderView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace ktplace {

namespace {

// Nets this large are listed but not drawn: a clock or reset net reaches most
// of the design, and its lines would cover the view.
constexpr std::size_t kMaxDrawnDegree = 200;

QString number(double v) {
    return QString::number(v, 'g', 10);
}

}  // namespace

std::optional<CellDetails> CellDetails::lookup(const ktSession &session, std::size_t id) {
    if (!session.hasDesign()) {
        return std::nullopt;
    }
    const Graph &g = session.graph();
    if (id >= g.getNumCells()) {
        return std::nullopt;
    }
    const Vertex &v = g.getCell(id);
    CellDetails d;
    d.id = id;
    d.name = QString::fromStdString(v.name);
    d.kind = v.isTerminal ? QStringLiteral("terminal")
                          : (v.isFixed ? QStringLiteral("fixed") : QStringLiteral("movable"));
    d.x = v.x;
    d.y = v.y;
    d.width = v.width;
    d.height = v.height;
    d.region = v.regionId;

    for (const std::size_t pinId : g.getCellPins(id)) {
        const std::size_t netId = g.getPin(pinId).netId;
        const auto &netPins = g.getNetPins(netId);
        NetRef ref;
        ref.name = QString::fromStdString(g.getNet(netId).name);
        ref.degree = netPins.size();
        if (ref.degree <= kMaxDrawnDegree) {
            for (const std::size_t other : netPins) {
                const std::size_t cellId = g.getPin(other).cellId;
                if (cellId == id) {
                    continue;
                }
                const Vertex &o = g.getCell(cellId);
                ref.others.emplace_back(o.x + o.width / 2.0, o.y + o.height / 2.0);
            }
        }
        d.nets.push_back(std::move(ref));
    }
    return d;
}

PropertyPanel::PropertyPanel(QWidget *parent) : QWidget(parent) {
    tree_ = new QTreeWidget(this);
    tree_->setColumnCount(2);
    tree_->setHeaderLabels({QStringLiteral("Property"), QStringLiteral("Value")});
    tree_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    tree_->setRootIsDecorated(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(tree_);
    clear();
}

void PropertyPanel::clear() {
    tree_->clear();
    auto *hint = new QTreeWidgetItem(tree_, {QStringLiteral("Click a cell in the layout view")});
    hint->setFirstColumnSpanned(true);
    hint->setDisabled(true);
}

void PropertyPanel::showCell(const CellDetails &cell) {
    tree_->clear();
    const auto add = [this](const QString &key, const QString &value) {
        return new QTreeWidgetItem(tree_, {key, value});
    };
    add(QStringLiteral("name"), cell.name);
    add(QStringLiteral("id"), QString::number(cell.id));
    add(QStringLiteral("kind"), cell.kind);
    add(QStringLiteral("x"), number(cell.x));
    add(QStringLiteral("y"), number(cell.y));
    add(QStringLiteral("width"), number(cell.width));
    add(QStringLiteral("height"), number(cell.height));
    if (cell.region >= 0) {
        add(QStringLiteral("fence region"), QString::number(cell.region));
    }
    QTreeWidgetItem *nets = add(QStringLiteral("nets"), QString::number(cell.nets.size()));
    for (const CellDetails::NetRef &n : cell.nets) {
        new QTreeWidgetItem(nets, {n.name, QStringLiteral("%1 pins").arg(n.degree)});
    }
    nets->setExpanded(cell.nets.size() <= 20);
}

}  // namespace ktplace
