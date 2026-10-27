#pragma once

#include "detailPlacer/abcdplace/abcdplace_dm.h"
#include "detailPlacer/abcdplace/abcdplace_options.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace ktplace::abcdplace {

class AbcdPlacer {
public:
    explicit AbcdPlacer(Design &design);
    ~AbcdPlacer();

    AbcdPlacer(const AbcdPlacer &) = delete;
    AbcdPlacer &operator=(const AbcdPlacer &) = delete;

    void place(const AbcdplaceOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

[[nodiscard]] std::vector<std::size_t> solveAssignment(const std::vector<double> &cost,
                                                       std::size_t n);

}  // namespace ktplace::abcdplace
