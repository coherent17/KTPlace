#pragma once

#include "detailPlacer/fastdp/fastdp_dm.h"
#include "detailPlacer/fastdp/fastdp_options.h"

#include <memory>

namespace ktplace::fastdp {

class FastDetailedPlacer {
public:
    explicit FastDetailedPlacer(Design &design);
    ~FastDetailedPlacer();

    FastDetailedPlacer(const FastDetailedPlacer &) = delete;
    FastDetailedPlacer &operator=(const FastDetailedPlacer &) = delete;

    FastdpResult place(const FastdpOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::fastdp
