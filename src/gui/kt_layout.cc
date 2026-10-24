#include "gui/kt_layout.h"

#include "datamodel/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

namespace ktplace::gui {

namespace {

constexpr float macroAreaInTypicalCells = 16.0F;

float median(std::vector<float> values) {
    if (values.empty()) {
        return 1.0F;
    }
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    return *mid > 0.0F ? *mid : 1.0F;
}

}  // namespace

std::shared_ptr<const LayoutGeometry> LayoutGeometry::of(const ktDM &db) {
    auto geometry = std::make_shared<LayoutGeometry>();
    const Graph &graph = db.getGraph();
    const std::size_t n = graph.getNumCells();
    geometry->name.reserve(n);
    geometry->width.reserve(n);
    geometry->height.reserve(n);
    geometry->kind.reserve(n);

    std::vector<float> movableWidths;
    std::vector<float> movableHeights;
    for (std::size_t v = 0; v < n; ++v) {
        const Vertex &cell = graph.getCell(v);
        geometry->name.push_back(cell.name);
        geometry->width.push_back(static_cast<float>(cell.width));
        geometry->height.push_back(static_cast<float>(cell.height));
        geometry->kind.push_back(cell.isFixed ? CellKind::Macro : CellKind::Movable);
        geometry->fenceOf.push_back(cell.regionId);
        if (!cell.isFixed) {
            movableWidths.push_back(static_cast<float>(cell.width));
            movableHeights.push_back(static_cast<float>(cell.height));
        }
    }
    geometry->typicalWidth = median(movableWidths);
    const float macroArea =
        macroAreaInTypicalCells * geometry->typicalWidth * median(std::move(movableHeights));
    for (std::size_t v = 0; v < n; ++v) {
        if (graph.getCell(v).isTerminal && geometry->width[v] * geometry->height[v] <= macroArea) {
            geometry->kind[v] = CellKind::Pad;
        }
    }

    for (const RowInfo &row : db.getRows()) {
        geometry->rows.push_back({static_cast<float>(row.coordinate),
                                  static_cast<float>(row.height), static_cast<float>(row.xlo()),
                                  static_cast<float>(row.xhi())});
    }
    geometry->die = db.placementDieBox();
    for (const Region &region : db.constraints().regions()) {
        LayoutGeometry::Fence &fence = geometry->fences.emplace_back();
        fence.name = region.name;
        for (const Rect &rect : region.rects) {
            fence.rects.push_back({rect.lo.x, rect.lo.y, rect.hi.x, rect.hi.y});
        }
    }

    geometry->netPinStart.push_back(0);
    geometry->netName.reserve(graph.getNumNets());
    for (std::size_t net = 0; net < graph.getNumNets(); ++net) {
        geometry->netName.push_back(graph.getNet(net).name);
        for (const std::size_t pinId : graph.getNetPins(net)) {
            const Pin &pin = graph.getPin(pinId);
            geometry->pinCell.push_back(static_cast<std::uint32_t>(pin.cellId));
            geometry->pinDx.push_back(static_cast<float>(pin.offsetX));
            geometry->pinDy.push_back(static_cast<float>(pin.offsetY));
        }
        geometry->netPinStart.push_back(static_cast<std::uint32_t>(geometry->pinCell.size()));
    }
    geometry->cellNetStart.push_back(0);
    for (std::size_t v = 0; v < n; ++v) {
        const std::size_t first = geometry->cellNet.size();
        for (const std::size_t pinId : graph.getCellPins(v)) {
            const auto net = static_cast<std::uint32_t>(graph.getPin(pinId).netId);
            if (std::find(geometry->cellNet.begin() + static_cast<std::ptrdiff_t>(first),
                          geometry->cellNet.end(), net) == geometry->cellNet.end()) {
                geometry->cellNet.push_back(net);
            }
        }
        geometry->cellNetStart.push_back(static_cast<std::uint32_t>(geometry->cellNet.size()));
    }
    return geometry;
}

LayoutFrame LayoutFrame::committed(std::shared_ptr<const LayoutGeometry> geometry, const ktDM &db,
                                   std::string stage) {
    LayoutFrame frame;
    const Graph &graph = db.getGraph();
    const std::size_t n = graph.getNumCells();
    frame.x.reserve(n);
    frame.y.reserve(n);
    for (std::size_t v = 0; v < n; ++v) {
        frame.x.push_back(static_cast<float>(graph.getCell(v).x));
        frame.y.push_back(static_cast<float>(graph.getCell(v).y));
    }
    frame.geometry = std::move(geometry);
    frame.hpwl = db.hpwl();
    frame.stage = std::move(stage);
    return frame;
}

bool LayoutFrame::drawable() const {
    return geometry && x.size() == geometry->width.size() && y.size() == x.size();
}

void Bounds::include(double px, double py) {
    xlo = std::min(xlo, px);
    ylo = std::min(ylo, py);
    xhi = std::max(xhi, px);
    yhi = std::max(yhi, py);
}

void Bounds::include(const Bounds &other) {
    include(other.xlo, other.ylo);
    include(other.xhi, other.yhi);
}

double Bounds::halfPerimeter() const {
    return (xhi - xlo) + (yhi - ylo);
}

Bounds LayoutFrame::cellBounds(std::uint32_t cell) const {
    return {x[cell], y[cell], x[cell] + geometry->width[cell], y[cell] + geometry->height[cell]};
}

std::optional<Bounds> LayoutFrame::cellsBounds(std::span<const std::uint32_t> cells) const {
    std::optional<Bounds> box;
    for (const std::uint32_t cell : cells) {
        const Bounds own = cellBounds(cell);
        if (box) {
            box->include(own);
        } else {
            box = own;
        }
    }
    return box;
}

std::optional<Bounds> LayoutFrame::netBounds(std::uint32_t net) const {
    const LayoutGeometry &g = *geometry;
    std::optional<Bounds> box;
    for (std::uint32_t pin = g.netPinStart[net]; pin < g.netPinStart[net + 1]; ++pin) {
        const std::uint32_t owner = g.pinCell[pin];
        const double px = x[owner] + g.pinDx[pin];
        const double py = y[owner] + g.pinDy[pin];
        if (box) {
            box->include(px, py);
        } else {
            box = Bounds{px, py, px, py};
        }
    }
    return box;
}

std::optional<Bounds> LayoutFrame::netsBounds(std::span<const std::uint32_t> nets) const {
    std::optional<Bounds> box;
    for (const std::uint32_t net : nets) {
        if (const std::optional<Bounds> own = netBounds(net)) {
            if (box) {
                box->include(*own);
            } else {
                box = own;
            }
        }
    }
    return box;
}

}  // namespace ktplace::gui
