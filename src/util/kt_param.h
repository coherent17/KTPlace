// @file kt_param.h
// The KTPLACE_* tuning parameters, declared once, with one place to read them

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ktplace {

/// What a parameter holds. The C++ type behind each is the alternative of
/// ParamValue with the same position: bool, std::int64_t, std::size_t, double,
/// std::string.
enum class ParamType { Bool, Int, Size, Double, String };

using ParamValue = std::variant<bool, std::int64_t, std::size_t, double, std::string>;

/// How the environment variable's text becomes a value.
///
/// Each style is exactly what the call site did before the registry existed, so
/// a run tuned through the environment behaves as it always has. They are not
/// unified on purpose: the sites disagree about garbage input, and changing that
/// would silently change existing runs. The one exception is a bool: under
/// either number rule, true/false, on/off and yes/no (any case) are read as the
/// words they are, since reading "true" as 0 turned KTPLACE_SIMPL_FENCES=true
/// into "fences off" and KTPLACE_ANIM=false into "animation on". An explicit
/// set() does not use these; it parses strictly and rejects anything malformed.
enum class EnvParse {
    /// std::atoi / std::atoll / std::atof: the leading number, and text that is
    /// not a number reads as 0. A bool is a word (see below) or "the number is
    /// non-zero".
    CNumber,
    /// std::stod: text that is not a number, or is out of range, leaves the
    /// default. A bool is a word (see below) or "the number is non-zero"; an
    /// integer is the double truncated. The plot settings read their variables
    /// this way.
    NumberOrDefault,
    /// String only: the raw text, validated (with any warning) by the site.
    Text,
};

/// Where a parameter's current value came from, in precedence order.
enum class ParamSource { Default, Environment, Explicit };

/// One declared parameter.
struct ParamDef {
    std::string_view name;   ///< the environment variable, e.g. KTPLACE_ANIM
    std::string_view group;  ///< component, e.g. "visualization", "simpl"
    std::string_view description;
    ParamType type = ParamType::Bool;
    ParamValue defaultValue;
    EnvParse envParse = EnvParse::CNumber;
    /// Range an explicit set() must satisfy (numeric types only).
    std::optional<double> minValue;
    std::optional<double> maxValue;
    bool minExclusive = false;  ///< the bound is "> min", not ">= min"
    /// Values 0 is allowed to take outside [min, max], for "0 = automatic".
    bool zeroAllowed = false;
    /// Accepted values of a string parameter (empty: any text).
    std::vector<std::string_view> choices;
};

/// Why a lookup or an explicit set() failed. Nothing here ends the process.
struct ParamError {
    enum class Kind { UnknownName, Malformed, OutOfRange, NotAChoice };
    Kind kind;
    std::string message;
};

/// A parameter's current state, for report_params and a settings dialog.
struct ParamState {
    const ParamDef *def = nullptr;
    ParamValue value;
    ParamSource source = ParamSource::Default;
    std::string text;  ///< value formatted as set() would accept it
};

/// The process-wide parameter registry.
///
/// Precedence: an explicit set() (Tcl, the GUI) beats the environment, which
/// beats the declared default. The environment is read at the time of each
/// read, not cached, so setenv() in a test takes effect at once.
///
/// Reads are thread-safe against each other and against set()/reset() (a
/// shared mutex guards the explicit values). They are not free -- a lock, an
/// environment lookup and a parse -- so a hot loop reads once into a local, which is also
/// the existing pattern: every site reads its value once per run or per stage.
/// As with the C environment itself, calling setenv() while another thread reads is a race.
class ParamRegistry {
public:
    static ParamRegistry &instance();

    ParamRegistry(const ParamRegistry &) = delete;
    ParamRegistry &operator=(const ParamRegistry &) = delete;

    /// Every declared parameter, in declaration order.
    [[nodiscard]] const std::vector<ParamDef> &definitions() const;

    /// The declaration of `name`, or nullptr when no such parameter exists.
    [[nodiscard]] const ParamDef *find(std::string_view name) const;

    /// The declaration of `name`, or an UnknownName error naming it.
    [[nodiscard]] std::expected<const ParamDef *, ParamError> lookup(std::string_view name) const;

    /// The value in effect: explicit, else environment, else default.
    /// T must be the parameter's own C++ type; a mismatch or an unknown name is
    /// a programming error and throws std::logic_error.
    template <typename T>
    [[nodiscard]] T get(std::string_view name) const {
        return std::get<T>(value(checked<T>(name)));
    }

    /// The value only when something overrides the default (an explicit set,
    /// or an environment variable that parsed). This is the shape of every
    /// existing "if the variable is set, params.x = ..." site, whose default lives in
    /// the stage's own params struct rather than here.
    template <typename T>
    [[nodiscard]] std::optional<T> overridden(std::string_view name) const {
        const std::optional<ParamValue> v = overrideOf(checked<T>(name));
        if (!v) {
            return std::nullopt;
        }
        return std::get<T>(*v);
    }

    /// Current state of one parameter / of every parameter in declaration order.
    [[nodiscard]] std::expected<ParamState, ParamError> state(std::string_view name) const;
    [[nodiscard]] std::vector<ParamState> list() const;

    /// Set from text (Tcl set_param, a GUI field). The text is parsed strictly
    /// for the parameter's type and checked against its range or choices; on any
    /// error the previous value is kept and the error is returned.
    std::expected<void, ParamError> set(std::string_view name, std::string_view text);

    /// Drop an explicit value, so the environment or the default applies again.
    std::expected<void, ParamError> reset(std::string_view name);
    void resetAll();

    /// Formats a value the way set() parses it ("true"/"false" for a bool).
    [[nodiscard]] static std::string format(const ParamValue &value);
    [[nodiscard]] static std::string_view typeName(ParamType type);
    [[nodiscard]] static std::string_view sourceName(ParamSource source);

private:
    ParamRegistry();

    template <typename T>
    [[nodiscard]] const ParamDef &checked(std::string_view name) const {
        const ParamDef &def = require(name);
        if (!std::holds_alternative<T>(def.defaultValue)) {
            typeMismatch(def);
        }
        return def;
    }

    [[nodiscard]] const ParamDef &require(std::string_view name) const;
    [[noreturn]] static void typeMismatch(const ParamDef &def);
    [[nodiscard]] ParamValue value(const ParamDef &def) const;
    [[nodiscard]] std::optional<ParamValue> overrideOf(const ParamDef &def) const;
    [[nodiscard]] ParamState stateOf(const ParamDef &def) const;

    std::vector<ParamDef> defs_;
    std::unordered_map<std::string_view, std::size_t> index_;
    mutable std::shared_mutex mutex_;
    std::map<std::string_view, ParamValue> explicit_;
};

/// Shorthands for ParamRegistry::instance().get / .overridden.
template <typename T>
[[nodiscard]] T param(std::string_view name) {
    return ParamRegistry::instance().get<T>(name);
}

template <typename T>
[[nodiscard]] std::optional<T> paramOverride(std::string_view name) {
    return ParamRegistry::instance().overridden<T>(name);
}

}  // namespace ktplace
