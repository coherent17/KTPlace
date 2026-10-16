#pragma once

#include "globalPlacer/simpl/simpl_design.h"
#include "globalPlacer/simpl/simpl_options.h"
#include "globalPlacer/simpl/simpl_placer.h"

namespace ktplace {

class ktDM;

namespace simpl {

class simplDM {
public:
    simplDM(Design design, SimplOptions options);

    [[nodiscard]] const SimplOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] SimplOptions &options() noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    void setFrameHook(FrameHook hook) {
        d.onFrame = std::move(hook);
    }
    void setBoundsHook(BoundsHook hook) {
        d.onBounds = std::move(hook);
    }

    SimplResult run();
    void commit(ktDM &db) const;

private:
    Design d;
    SimplOptions opts;
};

[[nodiscard]] simplDM buildSimplDM(const ktDM &db);

}  // namespace simpl
}  // namespace ktplace
