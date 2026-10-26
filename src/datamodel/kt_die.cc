// @file kt_die.cc

#include "datamodel/kt_die.h"

#include <limits>

namespace ktplace {

void DieInfo::setDieArea(const BBox &box) {
    area = box;
}

std::size_t DieInfo::addRow(double coordinate, double height, double sitewidth,
                            double sitespacing) {
    RowInfo row;
    row.coordinate = coordinate;
    row.height = height;
    row.sitewidth = sitewidth;
    row.sitespacing = sitespacing;
    rows.push_back(std::move(row));
    return rows.size() - 1;
}

std::size_t DieInfo::addSubrow(std::size_t rowId, double originX, double numSites) {
    if (rowId >= rows.size()) {
        return static_cast<std::size_t>(-1);
    }
    rows[rowId].subrows.push_back(SubrowInfo{originX, numSites});
    return rows[rowId].subrows.size() - 1;
}

const std::vector<RowInfo> &DieInfo::getRows() const {
    return rows;
}

std::size_t DieInfo::getNumRows() const {
    return rows.size();
}

void DieInfo::clear() {
    area = {};
    rows.clear();
}

BBox getPlacementDieBox(const DieInfo &die, const Graph &graph) {
    const double inf = std::numeric_limits<double>::max();
    // The fixed cells: the I/O pad ring bounds the die in a Bookshelf design.
    BBox box{inf, inf, -inf, -inf};
    const std::size_t nv = graph.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph.getCell(v);
        if (!vert.isFixed && !vert.isTerminal) {
            continue;
        }
        box.extend(BBox::of(vert.x, vert.y, std::max(vert.width, 1.0), std::max(vert.height, 1.0)));
    }
    const bool haveFixed = (box.xhi > box.xlo) && (box.yhi > box.ylo);

    // A declared die area, when the format carries one and it contains every
    // fixed cell. A declared area that excludes a fixed cell is not describing the
    // same die the pads describe, so it is not trusted.
    const BBox &da = die.getDieArea();
    if (da.xhi > da.xlo && da.yhi > da.ylo) {
        bool contains = true;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = graph.getCell(v);
            if (!vert.isFixed) {
                continue;
            }
            if (vert.x < da.xlo - 1.0 || vert.y < da.ylo - 1.0 ||
                vert.x + vert.width > da.xhi + 1.0 || vert.y + vert.height > da.yhi + 1.0) {
                contains = false;
                break;
            }
        }
        if (contains) {
            box = da;
        }
    }

    // The rows, unioned in. A cell in a row is legal by definition of a row.
    BBox rowBox{inf, inf, -inf, -inf};
    bool anyRow = false;
    for (const RowInfo &ri : die.getRows()) {
        if (!(ri.getPitch() > 0.0)) {
            continue;
        }
        rowBox.extend(BBox{ri.getXlo(), ri.coordinate, ri.getXhi(), ri.coordinate + ri.height});
        anyRow = true;
    }
    if (anyRow && (rowBox.xhi > rowBox.xlo) && (rowBox.yhi > rowBox.ylo)) {
        if (!haveFixed) {
            box = rowBox;
        } else {
            box.extend(rowBox);
        }
    }
    return box;
}

}  // namespace ktplace
