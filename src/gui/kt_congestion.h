#pragma once

#include "datamodel/kt_routingDemand.h"
#include "gui/kt_layout.h"

#include <cstddef>
#include <vector>

namespace ktplace::gui {

using DemandMap = RoutingDemand;

[[nodiscard]] DemandMap estimateDemand(const LayoutGeometry &geometry, const std::vector<float> &x,
                                       const std::vector<float> &y, std::size_t binsAcross);

}  // namespace ktplace::gui
