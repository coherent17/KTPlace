#pragma once

#include "datamodel/kt_graph.h"
#include "datamodel/kt_macroPlan.h"
#include "datamodel/kt_solutionMgr.h"
#include "macroPlacer/dmp/dmp_options.h"

#include <array>
#include <cstddef>
#include <vector>

namespace ktplace {
class ktDM;
}  // namespace ktplace

namespace ktplace::dmp {

inline constexpr int noMacro = -1;

struct Macro {
    std::size_t cellId = 0;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    Orient orient = Orient::N;
    bool movable = false;
    double originX = 0.0;
    double originY = 0.0;
    Orient originOrient = Orient::N;
};

struct Pin {
    int macro = noMacro;
    double x = 0.0;
    double y = 0.0;
};

struct Net {
    std::vector<std::size_t> pins;
};

struct Grid {
    double originX = 0.0;
    double pitchX = 0.0;
    double originY = 0.0;
    double pitchY = 0.0;
};

struct Design {
    std::vector<Macro> macros;
    std::vector<Pin> pins;
    std::vector<Net> nets;
    std::array<double, 4> die{};
    MacroRules rules;
    Grid grid;
};

struct DmpResult {
    std::size_t passes = 0;
    std::size_t moved = 0;
    std::size_t flipped = 0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

class dmpDM {
public:
    dmpDM(Design design, DmpOptions options);

    [[nodiscard]] const DmpOptions &options() const noexcept {
        return opts;
    }
    [[nodiscard]] const Design &design() const noexcept {
        return d;
    }

    DmpResult run();
    [[nodiscard]] dmpSolution solution() const;

private:
    Design d;
    DmpOptions opts;
};

[[nodiscard]] dmpDM buildDmpDM(const ktDM &db);

}  // namespace ktplace::dmp
