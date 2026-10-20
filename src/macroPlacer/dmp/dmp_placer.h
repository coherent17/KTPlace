#pragma once

#include "macroPlacer/dmp/dmp_dm.h"
#include "macroPlacer/dmp/dmp_options.h"

namespace ktplace::dmp {

[[nodiscard]] double macroNetHpwl(const Design &design);

[[nodiscard]] DmpResult placeMacros(Design &design, const DmpOptions &options);

}  // namespace ktplace::dmp
