#pragma once

#include "globalPlacer/ntuplace1/ntuplace1_dm.h"
#include "globalPlacer/ntuplace1/ntuplace1_options.h"

#include <memory>

namespace ktplace::ntuplace1 {

class RatioPlacer {
public:
    explicit RatioPlacer(Design &design);
    ~RatioPlacer();

    RatioPlacer(const RatioPlacer &) = delete;
    RatioPlacer &operator=(const RatioPlacer &) = delete;
    RatioPlacer(RatioPlacer &&) noexcept;
    RatioPlacer &operator=(RatioPlacer &&) noexcept;

    Ntuplace1Result place(const Ntuplace1Options &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::ntuplace1
