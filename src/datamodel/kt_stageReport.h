// @file kt_stageReport.h
// What a stage copied out of the ktDM, reported when its own database is built

#pragma once

#include "datamodel/kt_optionDb.h"
#include "util/kt_log.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace ktplace {

struct StageCounts {
    std::size_t cells = 0;
    std::size_t movable = 0;
    std::size_t nets = 0;
    std::size_t pins = 0;
    std::size_t rows = 0;
};

template <typename Options>
void reportStageBuild(const StageCounts &n, std::uint32_t checksum, const Options &options) {
    ktlog.beginStep(std::string(toString(Options::component)));
    ktlog.echo(
        "database built from ktDM {:08x}: {} cells ({} movable, {} fixed), {} nets, {} pins, {} "
        "rows",
        checksum, n.cells, n.movable, n.cells - n.movable, n.nets, n.pins, n.rows);
    reportOptions(options);
}

}  // namespace ktplace
