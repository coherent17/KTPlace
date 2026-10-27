#pragma once

#include "globalPlacer/kraftwerk2/kraftwerk2_dm.h"
#include "globalPlacer/kraftwerk2/kraftwerk2_options.h"

#include <memory>

namespace ktplace::kraftwerk2 {

class Kraftwerk2Placer {
public:
    explicit Kraftwerk2Placer(Design &design);
    ~Kraftwerk2Placer();

    Kraftwerk2Placer(const Kraftwerk2Placer &) = delete;
    Kraftwerk2Placer &operator=(const Kraftwerk2Placer &) = delete;

    void place(const Kraftwerk2Options &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::kraftwerk2
