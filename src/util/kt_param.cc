// @file kt_param.cc
// The KTPLACE_* parameter declarations, and how their values are resolved

#include "util/kt_param.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>

namespace ktplace {

namespace {

// Builders, so each declaration below reads as one line of intent and the
// default is constructed with the right variant alternative (a bare 0 or a
// string literal would otherwise pick the wrong one).
struct Range {
    std::optional<double> min;
    std::optional<double> max;
    bool minExclusive = false;
    bool zeroAllowed = false;
};

ParamDef boolean(std::string_view name, std::string_view group, bool def, EnvParse env,
                 std::string_view desc) {
    ParamDef d;
    d.name = name;
    d.group = group;
    d.description = desc;
    d.type = ParamType::Bool;
    d.defaultValue = def;
    d.envParse = env;
    return d;
}

ParamDef numeric(std::string_view name, std::string_view group, ParamType type, ParamValue def,
                 EnvParse env, Range range, std::string_view desc) {
    ParamDef d;
    d.name = name;
    d.group = group;
    d.description = desc;
    d.type = type;
    d.defaultValue = std::move(def);
    d.envParse = env;
    d.minValue = range.min;
    d.maxValue = range.max;
    d.minExclusive = range.minExclusive;
    d.zeroAllowed = range.zeroAllowed;
    return d;
}

ParamDef integer(std::string_view name, std::string_view group, std::int64_t def, EnvParse env,
                 Range range, std::string_view desc) {
    return numeric(name, group, ParamType::Int, ParamValue(std::in_place_type<std::int64_t>, def),
                   env, range, desc);
}

ParamDef size(std::string_view name, std::string_view group, std::size_t def, EnvParse env,
              Range range, std::string_view desc) {
    return numeric(name, group, ParamType::Size, ParamValue(std::in_place_type<std::size_t>, def),
                   env, range, desc);
}

ParamDef real(std::string_view name, std::string_view group, double def, EnvParse env, Range range,
              std::string_view desc) {
    return numeric(name, group, ParamType::Double, ParamValue(std::in_place_type<double>, def), env,
                   range, desc);
}

ParamDef text(std::string_view name, std::string_view group, std::string_view def,
              std::vector<std::string_view> choices, std::string_view desc) {
    ParamDef d;
    d.name = name;
    d.group = group;
    d.description = desc;
    d.type = ParamType::String;
    d.defaultValue = ParamValue(std::in_place_type<std::string>, def);
    d.envParse = EnvParse::Text;
    d.choices = std::move(choices);
    return d;
}

constexpr Range kAny{};
constexpr Range kNonNegative{0.0, std::nullopt, false, false};
constexpr Range kPositive{0.0, std::nullopt, true, false};

// The declarations. Defaults mirror the stage's own params struct (PlotOptions,
// SimplParams, RatioPlaceParams, LegalizeParams, DetailPlaceParams, or the
// literal at the read site); test_util checks that they still agree. The
// EnvParse of each is what its read site did before the registry.
std::vector<ParamDef> declarations() {
    using enum EnvParse;
    constexpr std::string_view viz = "visualization";
    constexpr std::string_view simpl = "simpl";
    constexpr std::string_view ntu = "ntuplace1";
    constexpr std::string_view abacus = "abacus";
    constexpr std::string_view fastdp = "fastdp";
    return {
        // --- visualization (PlotOptions::fromEnvironment) --------------------
        boolean("KTPLACE_ANIM", viz, true, NumberOrDefault,
                "record frames and assemble the placement GIF"),
        size("KTPLACE_ANIM_MAX_FRAMES", viz, 1200, NumberOrDefault, kNonNegative,
             "frames offered before the animator thins them"),
        size("KTPLACE_ANIM_MAX_BYTES", viz, std::size_t{96} << 20, NumberOrDefault, kNonNegative,
             "size cap of the assembled GIF, in bytes"),
        integer("KTPLACE_ANIM_DELAY_CS", viz, 12, NumberOrDefault, {0.0, 65535.0},
                "GIF frame delay, in hundredths of a second"),
        integer("KTPLACE_ANIM_BLEND", viz, 3, NumberOrDefault, kNonNegative,
                "in-between frames per placement frame"),
        real("KTPLACE_ANIM_ZOOM", viz, 2.0, NumberOrDefault, kPositive,
             "frame scale against the 768x768 frame size"),
        real("KTPLACE_FINAL_ZOOM", viz, 8.0, NumberOrDefault, kPositive,
             "scale of the final high-resolution still against 768x768"),
        boolean("KTPLACE_FINAL_PPM", viz, false, NumberOrDefault,
                "also write the lossless PPM beside the final PNG"),
        size("KTPLACE_ANIM_FINAL_HOLD", viz, 8, NumberOrDefault, kNonNegative,
             "extra GIF frames the finished placement is held for"),
        size("KTPLACE_ANIM_TRACE_EVERY", viz, 1, NumberOrDefault, kNonNegative,
             "global-placement frame every N iterations (0 = only the final frame)"),
        size("KTPLACE_ANIM_CG_EVERY", viz, 0, NumberOrDefault, kNonNegative,
             "frame every N conjugate-gradient iterations inside a solve (0 = off)"),
        boolean("KTPLACE_ANIM_DENSITY_MAPS", viz, true, NumberOrDefault,
                "write a bin-density heat map beside each placement frame"),

        // --- SimPL (kt_flowMgr.cc, simpl_placer.cc) --------------------------
        boolean("KTPLACE_SIMPL_FENCES", simpl, true, CNumber,
                "enforce the design's fence regions (0 shows what they cost)"),
        integer("KTPLACE_SIMPL_GRID", simpl, 0, CNumber, {8.0, 1024.0, false, true},
                "density bins per side, 8..1024 (0 = automatic, ~51 cells per bin)"),
        real("KTPLACE_SIMPL_CG_TOL", simpl, 1e-3, CNumber, kPositive,
             "relative residual that ends a conjugate-gradient solve"),
        text("KTPLACE_SIMPL_STRIPE_SCALE", simpl, "both", {"tight", "both", "none", "fill"},
             "look-ahead legalization stripe scaling mode"),
        text("KTPLACE_SIMPL_INIT_NET", simpl, "star", {"star", "b2b", "B2B"},
             "net model of the initial quadratic placement"),
        text("KTPLACE_SIMPL_LSS_NET", simpl, "b2b", {"star", "b2b", "B2B"},
             "net model of the lower-bound solves"),
        size("KTPLACE_SIMPL_INIT_ITERS", simpl, 7, CNumber, kNonNegative,
             "rounds of the initial quadratic placement"),
        integer("KTPLACE_SIMPL_ITERS", simpl, 100, CNumber, {1.0, std::nullopt},
                "global-placement iterations (values below 1 read as 1)"),
        size("KTPLACE_SIMPL_SEED", simpl, 20240607, CNumber, kNonNegative,
             "seed of the uniform initial placement"),
        real("KTPLACE_SIMPL_DENSITY", simpl, 1.0, CNumber, {0.05, 1.0},
             "target density g of look-ahead legalization (clamped to 0.05..1)"),
        real("KTPLACE_SIMPL_GLOBAL_CLUSTER", simpl, 0.5, CNumber, {0.0, 1.0},
             "fraction of the die a global overfilled cluster may grow to"),
        text("KTPLACE_SIMPL_PSEUDONET", simpl, "const", {"inv", "const"},
             "pseudonet weight law: const (paper) or inv (alpha / length)"),
        text("KTPLACE_SIMPL_START", simpl, "uniform", {"input", "auto", "uniform"},
             "starting placement: uniform (paper), input, or auto"),
        real("KTPLACE_SIMPL_ALPHA_BASE", simpl, 0.01, CNumber, kPositive,
             "pseudonet weight base: alpha = base * (1 + iteration)"),
        real("KTPLACE_SIMPL_ALPHA_DECAY", simpl, 1.0, CNumber, kPositive,
             "per-iteration alpha factor once overflow is below ALPHA_DECAY_BELOW"),
        real("KTPLACE_SIMPL_ALPHA_DECAY_BELOW", simpl, 0.40, CNumber, kNonNegative,
             "lower-bound overflow below which ALPHA_DECAY applies"),
        boolean("KTPLACE_SIMPL_NO_LAL", simpl, false, CNumber,
                "disable look-ahead legalization (diagnostic)"),
        integer("KTPLACE_SIMPL_LAL_ONLY", simpl, 0, CNumber, kNonNegative,
                "diagnostic: repeat look-ahead legalization N times on one input (0 = off)"),

        // --- NTUPlace1 (kt_flowMgr.cc) ---------------------------------------
        size("KTPLACE_NTU_LEAF_CELLS", ntu, 32, CNumber, kNonNegative,
             "partitioning stops at blocks of this many cells"),
        size("KTPLACE_NTU_MAX_LEVELS", ntu, 40, CNumber, kNonNegative,
             "maximum partitioning depth"),
        size("KTPLACE_NTU_RETRIES", ntu, 24, CNumber, kNonNegative,
             "ratio-cut retries per partition"),
        boolean("KTPLACE_NTU_VERBOSE", ntu, false, CNumber, "log every partitioning step"),

        // --- Abacus (kt_flowMgr.cc, abacus_legalizer.cc) ---------------------
        size("KTPLACE_ABACUS_MAX_ROW_DIST", abacus, 0, CNumber, kNonNegative,
             "rows searched either side of a cell's target row (0 = unlimited)"),
        real("KTPLACE_ABACUS_MIN_SUBROW_SITES", abacus, 1.0, CNumber, kNonNegative,
             "subrows narrower than this many sites are dropped"),
        real("KTPLACE_ABACUS_CLUSTER_GAP", abacus, 0.0, CNumber, kNonNegative,
             "cells this many sites apart or closer are merged into one cluster"),

        // --- FastDP (kt_flowMgr.cc, fastdp_placer.cc) ------------------------
        size("KTPLACE_DP_WINDOW", fastdp, 6, CNumber, {2.0, 14.0},
             "local re-ordering window; the subset DP costs 2^k per window"),
        boolean("KTPLACE_DP_NO_MEDIAN", fastdp, false, CNumber,
                "skip the median re-ordering sweep"),
    };
}

// --- environment parsing ----------------------------------------------------

// true/false, on/off, yes/no in any case, ignoring surrounding blanks; nullopt
// for anything else, which the number rules then read as before.
std::optional<bool> boolWord(std::string_view raw) {
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.front()))) {
        raw.remove_prefix(1);
    }
    while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
        raw.remove_suffix(1);
    }
    std::string l(raw);
    std::ranges::transform(l, l.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (l == "true" || l == "on" || l == "yes") {
        return true;
    }
    if (l == "false" || l == "off" || l == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<ParamValue> parseEnv(const ParamDef &def, const char *raw) {
    if (def.type == ParamType::Bool) {
        if (const auto word = boolWord(raw)) {
            return ParamValue(*word);
        }
    }
    switch (def.envParse) {
        case EnvParse::Text:
            return ParamValue(std::in_place_type<std::string>, raw);
        case EnvParse::CNumber:
            switch (def.type) {
                case ParamType::Bool:
                    return ParamValue(std::strtoll(raw, nullptr, 10) != 0);
                case ParamType::Int:
                    return ParamValue(std::in_place_type<std::int64_t>,
                                      std::strtoll(raw, nullptr, 10));
                case ParamType::Size:
                    // static_cast<std::size_t>(std::atoll(e)), as the sites did: a
                    // negative number wraps.
                    return ParamValue(std::in_place_type<std::size_t>,
                                      static_cast<std::size_t>(std::strtoll(raw, nullptr, 10)));
                case ParamType::Double:
                    return ParamValue(std::in_place_type<double>, std::strtod(raw, nullptr));
                case ParamType::String:
                    return ParamValue(std::in_place_type<std::string>, raw);
            }
            break;
        case EnvParse::NumberOrDefault: {
            double v = 0.0;
            try {
                v = std::stod(raw);
            } catch (const std::exception &) {
                return std::nullopt;
            }
            switch (def.type) {
                case ParamType::Bool:
                    return ParamValue(v != 0.0);
                case ParamType::Int:
                    return ParamValue(std::in_place_type<std::int64_t>,
                                      static_cast<std::int64_t>(static_cast<int>(v)));
                case ParamType::Size:
                    return ParamValue(std::in_place_type<std::size_t>, static_cast<std::size_t>(v));
                case ParamType::Double:
                    return ParamValue(std::in_place_type<double>, v);
                case ParamType::String:
                    return ParamValue(std::in_place_type<std::string>, raw);
            }
            break;
        }
    }
    return std::nullopt;
}

// --- strict parsing for set() -----------------------------------------------

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

ParamError malformed(const ParamDef &def, std::string_view textIn) {
    return {ParamError::Kind::Malformed, std::string(def.name) + ": '" + std::string(textIn) +
                                             "' is not a valid " +
                                             std::string(ParamRegistry::typeName(def.type))};
}

template <typename T>
std::optional<T> fromChars(std::string_view s) {
    T v{};
    const char *end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, v);
    if (ec != std::errc() || ptr != end) {
        return std::nullopt;
    }
    return v;
}

std::expected<ParamValue, ParamError> parseStrict(const ParamDef &def, std::string_view raw) {
    const std::string_view s = trim(raw);
    switch (def.type) {
        case ParamType::Bool: {
            const std::string l = lower(s);
            if (l == "1" || l == "true" || l == "on" || l == "yes") {
                return ParamValue(true);
            }
            if (l == "0" || l == "false" || l == "off" || l == "no") {
                return ParamValue(false);
            }
            return std::unexpected(malformed(def, raw));
        }
        case ParamType::Int:
            if (const auto v = fromChars<std::int64_t>(s)) {
                return ParamValue(std::in_place_type<std::int64_t>, *v);
            }
            return std::unexpected(malformed(def, raw));
        case ParamType::Size:
            // from_chars on an unsigned type already refuses a leading '-'.
            if (const auto v = fromChars<std::size_t>(s)) {
                return ParamValue(std::in_place_type<std::size_t>, *v);
            }
            return std::unexpected(malformed(def, raw));
        case ParamType::Double:
            if (const auto v = fromChars<double>(s); v && std::isfinite(*v)) {
                return ParamValue(std::in_place_type<double>, *v);
            }
            return std::unexpected(malformed(def, raw));
        case ParamType::String:
            return ParamValue(std::in_place_type<std::string>, std::string(s));
    }
    return std::unexpected(malformed(def, raw));
}

std::string formatBound(double v) {
    return ParamRegistry::format(ParamValue(v));
}

std::optional<ParamError> validate(const ParamDef &def, const ParamValue &v) {
    if (def.type == ParamType::String) {
        if (def.choices.empty()) {
            return std::nullopt;
        }
        const std::string &s = std::get<std::string>(v);
        if (std::ranges::find(def.choices, std::string_view(s)) != def.choices.end()) {
            return std::nullopt;
        }
        std::string list;
        for (const std::string_view c : def.choices) {
            list += (list.empty() ? "" : ", ") + std::string(c);
        }
        return ParamError{ParamError::Kind::NotAChoice,
                          std::string(def.name) + ": '" + s + "' is not one of: " + list};
    }
    if (def.type == ParamType::Bool) {
        return std::nullopt;
    }
    const double x = std::visit(
        [](const auto &a) -> double {
            using A = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<A, std::string>) {
                return 0.0;
            } else {
                return static_cast<double>(a);
            }
        },
        v);
    if (def.zeroAllowed && x == 0.0) {
        return std::nullopt;
    }
    const bool lowOk = !def.minValue || (def.minExclusive ? x > *def.minValue : x >= *def.minValue);
    const bool highOk = !def.maxValue || x <= *def.maxValue;
    if (lowOk && highOk) {
        return std::nullopt;
    }
    std::string range;
    if (def.minValue) {
        range += (def.minExclusive ? "> " : ">= ") + formatBound(*def.minValue);
    }
    if (def.maxValue) {
        range += (range.empty() ? "" : " and ") + std::string("<= ") + formatBound(*def.maxValue);
    }
    if (def.zeroAllowed) {
        range += ", or 0";
    }
    return ParamError{ParamError::Kind::OutOfRange, std::string(def.name) + ": " +
                                                        ParamRegistry::format(v) +
                                                        " is out of range (must be " + range + ")"};
}

ParamError unknown(std::string_view name) {
    return {ParamError::Kind::UnknownName, "unknown parameter '" + std::string(name) + "'"};
}

}  // namespace

// --- registry -----------------------------------------------------------------

ParamRegistry &ParamRegistry::instance() {
    static ParamRegistry registry;
    return registry;
}

ParamRegistry::ParamRegistry() : defs_(declarations()) {
    for (std::size_t i = 0; i < defs_.size(); ++i) {
        index_.emplace(defs_[i].name, i);
    }
}

const std::vector<ParamDef> &ParamRegistry::definitions() const {
    return defs_;
}

const ParamDef *ParamRegistry::find(std::string_view name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &defs_[it->second];
}

std::expected<const ParamDef *, ParamError> ParamRegistry::lookup(std::string_view name) const {
    if (const ParamDef *def = find(name)) {
        return def;
    }
    return std::unexpected(unknown(name));
}

const ParamDef &ParamRegistry::require(std::string_view name) const {
    if (const ParamDef *def = find(name)) {
        return *def;
    }
    throw std::logic_error("ktplace: read of undeclared parameter '" + std::string(name) + "'");
}

void ParamRegistry::typeMismatch(const ParamDef &def) {
    throw std::logic_error("ktplace: parameter '" + std::string(def.name) + "' is a " +
                           std::string(typeName(def.type)) + ", read as another type");
}

std::optional<ParamValue> ParamRegistry::overrideOf(const ParamDef &def) const {
    {
        const std::shared_lock lock(mutex_);
        if (const auto it = explicit_.find(def.name); it != explicit_.end()) {
            return it->second;
        }
    }
    // std::getenv is safe to call from several threads at once; only a
    // concurrent setenv() is not, and nothing in the engine calls that.
    const char *raw = std::getenv(std::string(def.name).c_str());  // NOLINT(concurrency-mt-unsafe)
    if (raw == nullptr) {
        return std::nullopt;
    }
    return parseEnv(def, raw);
}

ParamValue ParamRegistry::value(const ParamDef &def) const {
    if (std::optional<ParamValue> v = overrideOf(def)) {
        return std::move(*v);
    }
    return def.defaultValue;
}

ParamState ParamRegistry::stateOf(const ParamDef &def) const {
    ParamState st;
    st.def = &def;
    {
        const std::shared_lock lock(mutex_);
        if (const auto it = explicit_.find(def.name); it != explicit_.end()) {
            st.value = it->second;
            st.source = ParamSource::Explicit;
        }
    }
    if (st.source != ParamSource::Explicit) {
        const char *raw = std::getenv(std::string(def.name).c_str());  // NOLINT
        std::optional<ParamValue> env = raw ? parseEnv(def, raw) : std::nullopt;
        if (env) {
            st.value = std::move(*env);
            st.source = ParamSource::Environment;
        } else {
            st.value = def.defaultValue;
        }
    }
    st.text = format(st.value);
    return st;
}

std::expected<ParamState, ParamError> ParamRegistry::state(std::string_view name) const {
    const ParamDef *def = find(name);
    if (def == nullptr) {
        return std::unexpected(unknown(name));
    }
    return stateOf(*def);
}

std::vector<ParamState> ParamRegistry::list() const {
    std::vector<ParamState> out;
    out.reserve(defs_.size());
    for (const ParamDef &def : defs_) {
        out.push_back(stateOf(def));
    }
    return out;
}

std::expected<void, ParamError> ParamRegistry::set(std::string_view name, std::string_view textIn) {
    const ParamDef *def = find(name);
    if (def == nullptr) {
        return std::unexpected(unknown(name));
    }
    std::expected<ParamValue, ParamError> v = parseStrict(*def, textIn);
    if (!v) {
        return std::unexpected(std::move(v.error()));
    }
    if (std::optional<ParamError> err = validate(*def, *v)) {
        return std::unexpected(std::move(*err));
    }
    const std::unique_lock lock(mutex_);
    explicit_.insert_or_assign(def->name, std::move(*v));
    return {};
}

std::expected<void, ParamError> ParamRegistry::reset(std::string_view name) {
    const ParamDef *def = find(name);
    if (def == nullptr) {
        return std::unexpected(unknown(name));
    }
    const std::unique_lock lock(mutex_);
    explicit_.erase(def->name);
    return {};
}

void ParamRegistry::resetAll() {
    const std::unique_lock lock(mutex_);
    explicit_.clear();
}

std::string ParamRegistry::format(const ParamValue &value) {
    return std::visit(
        [](const auto &v) -> std::string {
            using V = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<V, bool>) {
                return v ? "true" : "false";
            } else if constexpr (std::is_same_v<V, std::string>) {
                return v;
            } else if constexpr (std::is_same_v<V, double>) {
                // Shortest text that reads back as the same double.
                std::array<char, 64> buf{};
                const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), v);
                return ec == std::errc() ? std::string(buf.data(), ptr) : std::to_string(v);
            } else {
                return std::to_string(v);
            }
        },
        value);
}

std::string_view ParamRegistry::typeName(ParamType type) {
    switch (type) {
        case ParamType::Bool:
            return "bool";
        case ParamType::Int:
            return "int";
        case ParamType::Size:
            return "size";
        case ParamType::Double:
            return "double";
        case ParamType::String:
            return "string";
    }
    return "unknown";
}

std::string_view ParamRegistry::sourceName(ParamSource source) {
    switch (source) {
        case ParamSource::Default:
            return "default";
        case ParamSource::Environment:
            return "env";
        case ParamSource::Explicit:
            return "set";
    }
    return "unknown";
}

}  // namespace ktplace
