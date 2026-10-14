// @file kt_optionDb.h
// Where a run's options live, addressed by component.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace ktplace {

// An option's value. The variant is the whole point: a script that writes 64, a
// script that writes 1e-6 and a script that writes a path stay distinguishable
// after parsing, so a component can ask for the type it expects and be told.
using OptionValue = std::variant<int, bool, double, std::string>;

// One component's configured options.
using OptionMap = std::unordered_map<std::string, OptionValue>;

// Where a value came from, so a report can say why an option is what it is.
enum class OptionOrigin {
    Default,  // never set: the option's own default stands
    Script,   // an add_option in the Tcl file
    Env,      // a KTPLACE_<COMPONENT>_<KEY> variable, as a fallback
};

[[nodiscard]] const char *toString(OptionOrigin origin);

// Every option a run has been given.
//
// What a script set (add_option) comes first. An option no script set falls
// back to the KTPLACE_* parameter behind it (util/kt_param.h): a set_param
// value, then the environment variable, parsed the way it always has been. Only
// then does the option's own default apply. The fallback is read live, so a
// set_param or a setenv takes effect at the next read.
//
// A process-wide instance because a run configures one thing and every component
// asks the same question; passing it around would be plumbing with no gain.
class OptionDb {
public:
    static OptionDb &instance();

    void set(const std::string &component, const std::string &key, OptionValue value,
             OptionOrigin origin = OptionOrigin::Script);
    void clear();

    // Null when the component was never mentioned, which is how a component tells
    // "not configured" from "configured to a blank string".
    [[nodiscard]] std::optional<OptionMap> optionsFor(const std::string &component) const;

    [[nodiscard]] std::optional<OptionValue> find(const std::string &component,
                                                  const std::string &key) const;
    [[nodiscard]] std::optional<OptionOrigin> originOf(const std::string &component,
                                                       const std::string &key) const;

    [[nodiscard]] std::vector<std::string> components() const;
    [[nodiscard]] std::size_t size() const noexcept {
        return total;
    }

    // A one-line-per-option report, printed once at startup: an option that
    // silently did not apply is the failure this exists to make visible.
    void report() const;

private:
    struct Entry {
        OptionValue value;
        OptionOrigin origin = OptionOrigin::Script;
    };

    std::unordered_map<std::string, std::unordered_map<std::string, Entry>> byComponent;
    std::size_t total = 0;
};

// A component's knowledge of one option: which component, which key, what type,
// and what it falls back to.
//
// Declaring these as typed constants gives a component compile-time knowledge of
// its own options, and keeps the key, the type and the default together where a
// reader can see them, instead of a getenv call with a default three lines away.
//
//     static constexpr Option<int> kGrid{"simpl", "grid", 64};
//     const int grid = kGrid.value();
//
// A value of the wrong type is reported rather than converted: a script that says
// `add_option simpl grid sixty-four` should not silently place a 0 in the grid.
template <typename T>
class Option {
public:
    Option(std::string componentName, std::string optionKey, T defaultValue)
        : component(std::move(componentName)),
          key(std::move(optionKey)),
          fallback(std::move(defaultValue)) {}

    [[nodiscard]] const std::string &componentName() const noexcept {
        return component;
    }
    [[nodiscard]] const std::string &optionKey() const noexcept {
        return key;
    }
    [[nodiscard]] const T &defaultValue() const noexcept {
        return fallback;
    }

    // Null when nothing set it, which is how a component tells an unconfigured
    // option from one configured to its default.
    [[nodiscard]] std::optional<T> tryGet() const;

    [[nodiscard]] T value() const {
        return tryGet().value_or(fallback);
    }

    [[nodiscard]] bool isSet() const {
        return tryGet().has_value();
    }

    [[nodiscard]] OptionOrigin origin() const {
        return OptionDb::instance().originOf(component, key).value_or(OptionOrigin::Default);
    }

private:
    std::string component;
    std::string key;
    T fallback;
};

// One option as the run will actually use it, beside the default it would have
// had. This is what a component's report prints, so a run's configuration can be
// read without holding the script and the source side by side.
struct ResolvedOption {
    std::string key;
    OptionValue value;     // what the run uses
    OptionValue fallback;  // the default, used when nothing set it
    OptionOrigin origin = OptionOrigin::Default;
};

// The per-component table: every option, its value, where that came from, and the
// default it falls back to.
void reportOptions(const std::string &component, const std::vector<ResolvedOption> &options);

// How T is spelled in the variant, and how to read a variant as T. Specialised
// per supported type; anything else is a compile error rather than a silent
// fallback.
template <typename T>
struct OptionTraits;

template <>
struct OptionTraits<int> {
    static OptionValue toValue(int value) {
        return value;
    }
    static std::optional<int> fromValue(const OptionValue &value) {
        if (const int *typed = std::get_if<int>(&value)) {
            return *typed;
        }
        // A double that is exactly an integer is still an integer to the caller:
        // Tcl has one number type and a script may reasonably write 64.0.
        if (const double *wide = std::get_if<double>(&value)) {
            return static_cast<int>(*wide);
        }
        return std::nullopt;
    }
};

template <>
struct OptionTraits<bool> {
    static OptionValue toValue(bool value) {
        return value;
    }
    static std::optional<bool> fromValue(const OptionValue &value) {
        if (const bool *typed = std::get_if<bool>(&value)) {
            return *typed;
        }
        // Tcl has one number type, so a script that writes 0 for a flag means
        // false and 1 means true. Reading it any other way would make the most
        // natural spelling of a flag do nothing at all.
        if (const int *narrow = std::get_if<int>(&value)) {
            return *narrow != 0;
        }
        if (const double *wide = std::get_if<double>(&value)) {
            return *wide != 0.0;
        }
        return std::nullopt;
    }
};

// Counts are stored as an int, the largest integer a Tcl script can write, and
// widened here: a script has no unsigned type, and a negative count is a mistake
// worth reporting at the point of use rather than wrapping silently.

template <>
struct OptionTraits<std::size_t> {
    static OptionValue toValue(std::size_t value) {
        return static_cast<int>(value);
    }
    static std::optional<std::size_t> fromValue(const OptionValue &value) {
        const auto narrow = OptionTraits<int>::fromValue(value);
        if (!narrow || *narrow < 0) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(*narrow);
    }
};


template <>
struct OptionTraits<double> {
    static OptionValue toValue(double value) {
        return value;
    }
    static std::optional<double> fromValue(const OptionValue &value) {
        if (const double *typed = std::get_if<double>(&value)) {
            return *typed;
        }
        if (const int *narrow = std::get_if<int>(&value)) {
            return static_cast<double>(*narrow);
        }
        return std::nullopt;
    }
};

template <>
struct OptionTraits<std::string> {
    static OptionValue toValue(std::string value) {
        return value;
    }
    static std::optional<std::string> fromValue(const OptionValue &value) {
        if (const std::string *typed = std::get_if<std::string>(&value)) {
            return *typed;
        }
        return std::nullopt;
    }
};

template <typename T>
std::optional<T> Option<T>::tryGet() const {
    const auto found = OptionDb::instance().find(component, key);
    if (!found) {
        return std::nullopt;
    }
    return OptionTraits<T>::fromValue(*found);
}

// Reads a declared option and records both what it resolved to and its default,
// so a component builds its report by declaring its options rather than by
// remembering to write them up twice.
template <typename T>
[[nodiscard]] ResolvedOption resolve(const Option<T> &option) {
    return ResolvedOption{option.optionKey(), OptionTraits<T>::toValue(option.value()),
                          OptionTraits<T>::toValue(option.defaultValue()), option.origin()};
}

}  // namespace ktplace
