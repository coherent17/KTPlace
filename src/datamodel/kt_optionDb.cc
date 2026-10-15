// @file kt_optionDb.cc
// Implementation of the option store

#include "datamodel/kt_optionDb.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <fmt/format.h>

namespace ktplace {

namespace {

std::string toText(const OptionValue &value) {
    return std::visit(
        [](const auto &held) -> std::string {
            using Held = std::decay_t<decltype(held)>;
            if constexpr (std::same_as<Held, bool>) {
                return held ? "1" : "0";
            } else {
                return fmt::format("{}", held);
            }
        },
        value);
}

}  // namespace

std::optional<Component> componentFromName(std::string_view name) noexcept {
    const auto found = std::ranges::find(kComponentNames, name);
    if (found == kComponentNames.end()) {
        return std::nullopt;
    }
    return static_cast<Component>(std::distance(kComponentNames.begin(), found));
}

ComponentOptions::ComponentOptions(Component component, OptionMap configured)
    : id(component), values(std::move(configured)) {}

std::optional<OptionValue> ComponentOptions::find(std::string_view key) const {
    const auto found = values.find(key);
    if (found == values.end()) {
        return std::nullopt;
    }
    return found->second;
}

void ComponentOptions::warnWrongType(std::string_view key, const OptionValue &value,
                                     std::string_view expected) const {
    ktlog.warning("option {} {}: expected {}, got '{}'; the default is used", toString(id), key,
                  expected, toText(value));
}

void reportOptionTable(Component component,
                       const std::vector<std::pair<std::string_view, OptionValue>> &rows) {
    if (rows.empty()) {
        return;
    }
    ktReportTable table(std::string(toString(component)) + " options");
    table.setHeaders({"option", "value"});
    for (const auto &[key, value] : rows) {
        table.addRow({std::string(key), toText(value)});
    }
    table.emit();
}

OptionDb &OptionDb::instance() {
    static OptionDb db;
    return db;
}

void OptionDb::set(Component component, std::string key, OptionValue value) {
    byComponent[std::to_underlying(component)].insert_or_assign(std::move(key), std::move(value));
}

void OptionDb::clear() noexcept {
    for (OptionMap &options : byComponent) {
        options.clear();
    }
}

ComponentOptions OptionDb::getComponentOption(Component component) const {
    return ComponentOptions(component, byComponent[std::to_underlying(component)]);
}

std::size_t OptionDb::size() const noexcept {
    std::size_t total = 0;
    for (const OptionMap &options : byComponent) {
        total += options.size();
    }
    return total;
}

void OptionDb::report() const {
    if (size() == 0) {
        return;
    }
    ktReportTable table("Configured options");
    table.setHeaders({"component", "option", "value"});
    for (std::size_t c = 0; c < byComponent.size(); ++c) {
        for (const auto &[key, value] : byComponent[c]) {
            table.addRow({std::string(kComponentNames[c]), key, toText(value)});
        }
    }
    table.emit();
}

}  // namespace ktplace
