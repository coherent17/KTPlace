// @file kt_propertyPanel.h
// The Properties panel: what is known about the selected cell.
//
// Filled from a CellDetails the engine looks up when a cell is picked (see
// MainWindow), so it shows the design as the last command left it.

#pragma once

#include <QPointF>
#include <QString>
#include <QWidget>
#include <cstddef>
#include <optional>
#include <vector>

class QTreeWidget;

namespace ktplace {

class ktSession;

/// One cell's record, looked up on the engine thread.
struct CellDetails {
    std::size_t id = 0;
    QString name;
    QString kind;  // "movable", "fixed", "terminal"
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    int region = -1;

    struct NetRef {
        QString name;
        std::size_t degree = 0;
        /// Centres of the other cells on the net, for the overlay.
        std::vector<QPointF> others;
    };
    std::vector<NetRef> nets;

    /// Look up cell @p id. Engine thread only; nullopt without a design or for
    /// an id out of range.
    [[nodiscard]] static std::optional<CellDetails> lookup(const ktSession &session,
                                                           std::size_t id);
};

class PropertyPanel : public QWidget {
public:
    explicit PropertyPanel(QWidget *parent = nullptr);

    void showCell(const CellDetails &cell);
    void clear();

    [[nodiscard]] QTreeWidget *tree() const {
        return tree_;
    }

private:
    QTreeWidget *tree_ = nullptr;
};

}  // namespace ktplace
