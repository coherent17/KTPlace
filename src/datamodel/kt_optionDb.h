// @file kt_optionDb.h
// Where a run's options live, addressed by component.

#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ktplace {

enum class Component : std::uint8_t {
    Simpl,
    NtuPlace1,
    Kraftwerk2,
    Abacus,
    MultiRow,
    FastDp,
    Visualizer,
    Flow
};

inline constexpr std::array<std::string_view, 8> componentNames{
    "simpl", "ntuplace1", "kraftwerk2", "abacus", "multirow", "fastdp", "visualizer", "flow"};

static_assert(componentNames.size() == std::to_underlying(Component::Flow) + 1);

[[nodiscard]] constexpr std::string_view toString(Component component) noexcept {
    return componentNames[std::to_underlying(component)];
}

[[nodiscard]] std::optional<Component> getComponentFromName(std::string_view name) noexcept;

using OptionValue = std::variant<int, bool, double, std::string>;
using OptionMap = std::map<std::string, OptionValue, std::less<>>;

template <typename T>
concept OptionType = std::same_as<T, int> || std::same_as<T, bool> || std::same_as<T, double> ||
                     std::same_as<T, std::size_t> || std::same_as<T, std::string>;

template <OptionType T>
[[nodiscard]] std::optional<T> convertOption(const OptionValue &value) {
    return std::visit(
        [](const auto &held) -> std::optional<T> {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::same_as<T, Held>) {
                return held;
            } else if constexpr (std::same_as<T, std::string> || std::same_as<Held, std::string>) {
                return std::nullopt;
            } else if constexpr (std::same_as<T, bool>) {
                return held != Held{};
            } else if constexpr (std::same_as<Held, bool>) {
                return std::nullopt;
            } else if constexpr (std::same_as<T, std::size_t>) {
                return held < 0 ? std::nullopt : std::optional<T>(static_cast<T>(held));
            } else {
                return static_cast<T>(held);
            }
        },
        value);
}

template <OptionType T>
[[nodiscard]] OptionValue toOptionValue(T value) {
    if constexpr (std::same_as<T, std::size_t>) {
        return static_cast<int>(value);
    } else {
        return value;
    }
}

template <OptionType T>
[[nodiscard]] constexpr std::string_view getOptionTypeName() noexcept {
    if constexpr (std::same_as<T, int>) {
        return "int";
    } else if constexpr (std::same_as<T, bool>) {
        return "bool";
    } else if constexpr (std::same_as<T, double>) {
        return "double";
    } else if constexpr (std::same_as<T, std::size_t>) {
        return "non-negative int";
    } else {
        return "string";
    }
}

class ComponentOptions {
public:
    explicit ComponentOptions(Component component, OptionMap configured = {});

    [[nodiscard]] bool empty() const noexcept {
        return values.empty();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return values.size();
    }
    [[nodiscard]] bool contains(std::string_view key) const {
        return values.contains(key);
    }

    template <OptionType T>
    [[nodiscard]] std::optional<T> get(std::string_view key) const {
        const auto found = values.find(key);
        if (found == values.end()) {
            return std::nullopt;
        }
        std::optional<T> typed = convertOption<T>(found->second);
        if (!typed) {
            warnWrongType(key, found->second, getOptionTypeName<T>());
        }
        return typed;
    }

private:
    void warnWrongType(std::string_view key, const OptionValue &value,
                       std::string_view expected) const;

    Component id;
    OptionMap values;
};

class OptionDb {
public:
    static OptionDb &getInstance();

    void set(Component component, std::string key, OptionValue value);
    void clear() noexcept;

    [[nodiscard]] ComponentOptions getComponentOption(Component component) const;
    [[nodiscard]] std::size_t size() const noexcept;

    void report() const;

private:
    std::array<OptionMap, componentNames.size()> byComponent;
};

template <typename Options, OptionType T>
struct OptionKey {
    std::string_view name;
    T Options::*member;
};

template <typename Options, OptionType T>
[[nodiscard]] constexpr OptionKey<Options, T> option(std::string_view name, T Options::*member) {
    return {name, member};
}

template <typename T>
concept ComponentOptionSet = requires {
    { T::component } -> std::convertible_to<Component>;
};

template <ComponentOptionSet Options>
[[nodiscard]] Options loadOptions() {
    const ComponentOptions configured =
        OptionDb::getInstance().getComponentOption(Options::component);
    Options out;
    std::apply(
        [&](const auto &...key) {
            ((out.*key.member =
                  configured.get<std::remove_cvref_t<decltype(out.*key.member)>>(key.name).value_or(
                      out.*key.member)),
             ...);
        },
        Options::getConfigurableOptions());
    return out;
}

void reportOptionTable(Component component,
                       const std::vector<std::pair<std::string_view, OptionValue>> &rows);

template <ComponentOptionSet Options>
void reportOptions(const Options &options) {
    std::vector<std::pair<std::string_view, OptionValue>> rows;
    std::apply(
        [&](const auto &...key) {
            (rows.emplace_back(key.name, toOptionValue(options.*key.member)), ...);
        },
        Options::getConfigurableOptions());
    reportOptionTable(Options::component, rows);
}

}  // namespace ktplace
