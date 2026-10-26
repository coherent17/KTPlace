#include "gui/kt_congestion.h"

namespace ktplace::gui {

DemandMap estimateDemand(const LayoutGeometry &geometry, const std::vector<float> &x,
                         const std::vector<float> &y, std::size_t binsAcross) {
    return getRoutingDemand(geometry.netPinStart, geometry.pinCell, geometry.pinDx, geometry.pinDy,
                            x, y, geometry.die, binsAcross);
}

}  // namespace ktplace::gui
