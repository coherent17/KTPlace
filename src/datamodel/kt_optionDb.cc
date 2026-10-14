// @file kt_optionDb.cc
// Implementation of the option store

#include "datamodel/kt_optionDb.h"

#include "util/kt_log.h"
#include "util/kt_param.h"
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

// Which KTPLACE_* parameter stands behind each option. The registry owns the
// environment (how each variable has always been parsed) and set_param; an
// option no script set falls back to whatever it says.
struct ParamLink {
    const char *component;
    const char *key;
    const char *param;
};

constexpr ParamLink kLinks[] = {
    {"simpl", "grid", "KTPLACE_SIMPL_GRID"},
    {"simpl", "cg_tol", "KTPLACE_SIMPL_CG_TOL"},
    {"simpl", "init_iters", "KTPLACE_SIMPL_INIT_ITERS"},
    {"simpl", "iters", "KTPLACE_SIMPL_ITERS"},
    {"simpl", "seed", "KTPLACE_SIMPL_SEED"},
    {"simpl", "init_net", "KTPLACE_SIMPL_INIT_NET"},
    {"simpl", "lss_net", "KTPLACE_SIMPL_LSS_NET"},
    {"simpl", "pseudonet", "KTPLACE_SIMPL_PSEUDONET"},
    {"simpl", "start", "KTPLACE_SIMPL_START"},
    {"simpl", "no_lal", "KTPLACE_SIMPL_NO_LAL"},
    {"simpl", "lal_only_rounds", "KTPLACE_SIMPL_LAL_ONLY"},
    {"simpl", "fences", "KTPLACE_SIMPL_FENCES"},
    {"simpl", "density", "KTPLACE_SIMPL_DENSITY"},
    {"simpl", "global_cluster", "KTPLACE_SIMPL_GLOBAL_CLUSTER"},
    {"simpl", "alpha_base", "KTPLACE_SIMPL_ALPHA_BASE"},
    {"simpl", "alpha_decay", "KTPLACE_SIMPL_ALPHA_DECAY"},
    {"simpl", "alpha_decay_below", "KTPLACE_SIMPL_ALPHA_DECAY_BELOW"},
    {"simpl", "stripe_scale", "KTPLACE_SIMPL_STRIPE_SCALE"},
    {"ntuplace1", "leaf_cells", "KTPLACE_NTU_LEAF_CELLS"},
    {"ntuplace1", "max_levels", "KTPLACE_NTU_MAX_LEVELS"},
    {"ntuplace1", "retries", "KTPLACE_NTU_RETRIES"},
    {"ntuplace1", "verbose", "KTPLACE_NTU_VERBOSE"},
    {"abacus", "max_row_dist", "KTPLACE_ABACUS_MAX_ROW_DIST"},
    {"abacus", "min_subrow_sites", "KTPLACE_ABACUS_MIN_SUBROW_SITES"},
    {"abacus", "cluster_gap", "KTPLACE_ABACUS_CLUSTER_GAP"},
    {"fastdp", "window", "KTPLACE_DP_WINDOW"},
    {"fastdp", "no_median", "KTPLACE_DP_NO_MEDIAN"},
    {"visualizer", "anim", "KTPLACE_ANIM"},
    {"visualizer", "anim_max_frames", "KTPLACE_ANIM_MAX_FRAMES"},
    {"visualizer", "anim_zoom", "KTPLACE_ANIM_ZOOM"},
    {"visualizer", "final_zoom", "KTPLACE_FINAL_ZOOM"},
    {"visualizer", "final_ppm", "KTPLACE_FINAL_PPM"},
    {"visualizer", "anim_max_bytes", "KTPLACE_ANIM_MAX_BYTES"},
    {"visualizer", "anim_delay_cs", "KTPLACE_ANIM_DELAY_CS"},
    {"visualizer", "anim_blend", "KTPLACE_ANIM_BLEND"},
    {"visualizer", "anim_final_hold", "KTPLACE_ANIM_FINAL_HOLD"},
    {"visualizer", "anim_trace_every", "KTPLACE_ANIM_TRACE_EVERY"},
    {"visualizer", "anim_cg_every", "KTPLACE_ANIM_CG_EVERY"},
    {"visualizer", "anim_density_maps", "KTPLACE_ANIM_DENSITY_MAPS"},
};

struct Fallback {
    OptionValue value;
    OptionOrigin origin;
};

// The registry's value for an option, when the environment or set_param
// overrides the registry's default; nothing otherwise, so the component's own
// default stands.
std::optional<Fallback> fallbackFor(const std::string &component, const std::string &key) {
    const auto link = std::ranges::find_if(kLinks, [&](const ParamLink &l) {
        return component == l.component && key == l.key;
    });
    if (link == std::end(kLinks)) {
        return std::nullopt;
    }
    const auto state = ParamRegistry::instance().state(link->param);
    if (!state || state->source == ParamSource::Default) {
        return std::nullopt;
    }
    const OptionOrigin origin =
        state->source == ParamSource::Explicit ? OptionOrigin::Script : OptionOrigin::Env;
    OptionValue value = std::visit(
        [](const auto &v) -> OptionValue {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, double> ||
                          std::is_same_v<T, std::string>) {
                return v;
            } else {
                // Counts and integers: the option store's integer is an int, as a
                // script's is.
                return static_cast<int>(v);
            }
        },
        state->value);
    // The one word the two spell differently: the registry's "const"/"inv" law
    // is the option's "constant"/"inverse".
    if (component == "simpl" && key == "pseudonet") {
        const std::string law = std::get<std::string>(value);
        value = law == "const" ? std::string("constant") : std::string("inverse");
    }
    return Fallback{std::move(value), origin};
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
    OptionMap out;
    if (found != byComponent.end()) {
        out.reserve(found->second.size());
        for (const auto &entry : found->second) {
            out.emplace(entry.first, entry.second.value);
        }
    }
    // What the environment or set_param says, for the keys no script set.
    for (const ParamLink &link : kLinks) {
        if (component == link.component && !out.contains(link.key)) {
            if (auto fallback = fallbackFor(component, link.key)) {
                out.emplace(link.key, std::move(fallback->value));
            }
        }
    }
    if (out.empty() && found == byComponent.end()) {
        return std::nullopt;
    }
    return out;
}

std::optional<OptionValue> OptionDb::find(const std::string &component,
                                          const std::string &key) const {
    const auto found = byComponent.find(component);
    if (found != byComponent.end()) {
        const auto entry = found->second.find(key);
        if (entry != found->second.end()) {
            return entry->second.value;
        }
    }
    if (auto fallback = fallbackFor(component, key)) {
        return std::move(fallback->value);
    }
    return std::nullopt;
}

std::optional<OptionOrigin> OptionDb::originOf(const std::string &component,
                                               const std::string &key) const {
    const auto found = byComponent.find(component);
    if (found != byComponent.end()) {
        const auto entry = found->second.find(key);
        if (entry != found->second.end()) {
            return entry->second.origin;
        }
    }
    if (const auto fallback = fallbackFor(component, key)) {
        return fallback->origin;
    }
    return std::nullopt;
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
