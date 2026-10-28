#pragma once

#include "legalizer/ripplelg/ripplelg_dm.h"
#include "legalizer/ripplelg/ripplelg_options.h"

#include <memory>

namespace ktplace::ripplelg {

class RippleLegalizer {
public:
    explicit RippleLegalizer(Design &design);
    ~RippleLegalizer();

    RippleLegalizer(const RippleLegalizer &) = delete;
    RippleLegalizer &operator=(const RippleLegalizer &) = delete;
    RippleLegalizer(RippleLegalizer &&) noexcept;
    RippleLegalizer &operator=(RippleLegalizer &&) noexcept;

    void legalize(const RipplelgOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::ripplelg
