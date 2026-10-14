// @file kt_optionDb.cc
// Implementation of the option store

#include "datamodel/kt_optionDb.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <cctype>
#include <fmt/format.h>

namespace ktplace {

namespace {

// The text a report prints for any of the variant's types.
std::string toText(const OptionValue &value) {
    if (const auto *asInt = std::get_if<int>(&value)) {
        return std::to_string(*asInt);
    }
    if (const auto *asBool = std::get_if<bool>(&value)) {
        return *asBool ? "1" : "0";
    }
    if (const auto *asDouble = std::get_if<double>(&value)) {
        return fmt::format("{}", *asDouble);
    }
    return std::get<std::string>(value);
}

}  // namespace

const char *toString(OptionOrigin origin) {
    switch (origin) {
        case OptionOrigin::Default:
            return "default";
        case OptionOrigin::Script:
            return "script";
        case OptionOrigin::Env:
            return "environment";
    }
    return "unknown";
}

OptionDb &OptionDb::instance() {
    static OptionDb db;
    return db;
}

void OptionDb::set(const std::string &component, const std::string &key, OptionValue value,
                   OptionOrigin origin) {
    auto &entries = byComponent[component];
    if (entries.find(key) == entries.end()) {
        ++total;
    }
    entries.insert_or_assign(key, Entry{std::move(value), origin});
}

void OptionDb::clear() {
    byComponent.clear();
    total = 0;
}

std::optional<OptionMap> OptionDb::optionsFor(const std::string &component) const {
    const auto found = byComponent.find(component);
    if (found == byComponent.end()) {
        return std::nullopt;
    }
    OptionMap out;
    out.reserve(found->second.size());
    for (const auto &entry : found->second) {
        out.emplace(entry.first, entry.second.value);
    }
    return out;
}

std::optional<OptionValue> OptionDb::find(const std::string &component,
                                          const std::string &key) const {
    const auto found = byComponent.find(component);
    if (found == byComponent.end()) {
        return std::nullopt;
    }
    const auto entry = found->second.find(key);
    if (entry == found->second.end()) {
        return std::nullopt;
    }
    return entry->second.value;
}

std::optional<OptionOrigin> OptionDb::originOf(const std::string &component,
                                               const std::string &key) const {
    const auto found = byComponent.find(component);
    if (found == byComponent.end()) {
        return std::nullopt;
    }
    const auto entry = found->second.find(key);
    if (entry == found->second.end()) {
        return std::nullopt;
    }
    return entry->second.origin;
}

std::vector<std::string> OptionDb::components() const {
    std::vector<std::string> names;
    names.reserve(byComponent.size());
    for (const auto &entry : byComponent) {
        names.push_back(entry.first);
    }
    std::sort(names.begin(), names.end());
    return names;
}

void OptionDb::report() const {
    if (byComponent.empty()) {
        return;
    }
    ktReportTable table("Configured options");
    table.setHeaders({"component", "option", "value", "from"});
    for (const std::string &name : components()) {
        std::vector<std::string> keys;
        for (const auto &entry : byComponent.at(name)) {
            keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());
        for (const std::string &key : keys) {
            const Entry &entry = byComponent.at(name).at(key);
            table.addRow({name, key, toText(entry.value), toString(entry.origin)});
        }
    }
    table.emit();
}

void reportOptions(const std::string &component, const std::vector<ResolvedOption> &options) {
    if (options.empty()) {
        return;
    }
    std::vector<const ResolvedOption *> ordered;
    ordered.reserve(options.size());
    for (const ResolvedOption &option : options) {
        ordered.push_back(&option);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const ResolvedOption *left, const ResolvedOption *right) {
                  return left->key < right->key;
              });

    ktReportTable table(component + " options");
    table.setHeaders({"option", "value", "default", "from"});
    for (const ResolvedOption *option : ordered) {
        table.addRow({option->key, toText(option->value), toText(option->fallback),
                      toString(option->origin)});
    }
    table.emit();
}

}  // namespace ktplace
