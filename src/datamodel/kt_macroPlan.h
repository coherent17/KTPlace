// @file kt_macroPlan.h
// The macros a detailed macro placement may move, where they started, and the rules it must keep.

#pragma once

#include "datamodel/kt_graph.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ktplace {

[[nodiscard]] constexpr bool mirrorsX(Orient orient) noexcept {
    return orient == Orient::FN || orient == Orient::S;
}

[[nodiscard]] constexpr bool mirrorsY(Orient orient) noexcept {
    return orient == Orient::FS || orient == Orient::S;
}

[[nodiscard]] constexpr Orient orientFromMirrors(bool x, bool y) noexcept {
    if (x) {
        return y ? Orient::S : Orient::FN;
    }
    return y ? Orient::FS : Orient::N;
}

inline constexpr std::array<std::pair<Orient, std::string_view>, 4> orientNames{
    {{Orient::N, "N"}, {Orient::S, "S"}, {Orient::FN, "FN"}, {Orient::FS, "FS"}}};

[[nodiscard]] constexpr std::string_view orientName(Orient orient) noexcept {
    for (const auto &[value, name] : orientNames) {
        if (value == orient) {
            return name;
        }
    }
    return "N";
}

[[nodiscard]] constexpr std::optional<Orient> orientFromName(std::string_view name) noexcept {
    for (const auto &[value, text] : orientNames) {
        if (text == name) {
            return value;
        }
    }
    return std::nullopt;
}

struct MacroRules {
    double maxDisplacement = 0.0;
    double minSpacing = 0.0;
    double halo = 0.0;
};

struct PlannedMacro {
    std::size_t cellId = 0;
    std::string master;
    bool movable = false;
    double originX = 0.0;
    double originY = 0.0;
    Orient originOrient = Orient::N;
};

struct MacroPlan {
    std::string version;
    std::string design;
    double dbuPerMicron = 1.0;
    std::vector<std::pair<double, double>> diePoints;
    std::array<double, 4> die{};
    MacroRules rules;
    std::vector<PlannedMacro> macros;

    [[nodiscard]] std::size_t movableCount() const {
        std::size_t n = 0;
        for (const PlannedMacro &macro : macros) {
            n += macro.movable ? 1 : 0;
        }
        return n;
    }
};

}  // namespace ktplace
