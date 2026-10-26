#pragma once

#include "globalPlacer/simpl/simpl_dm.h"
#include "globalPlacer/simpl/simpl_options.h"

#include <memory>

namespace ktplace::simpl {

class simplPlacer {
public:
    explicit simplPlacer(Design &design);
    ~simplPlacer();

    simplPlacer(const simplPlacer &) = delete;
    simplPlacer &operator=(const simplPlacer &) = delete;
    simplPlacer(simplPlacer &&) noexcept;
    simplPlacer &operator=(simplPlacer &&) noexcept;

    void place(const SimplOptions &options);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::simpl
