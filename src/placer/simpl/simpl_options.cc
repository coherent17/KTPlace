// @file simpl_options.cc
// Reading SimPL's options from the option store

#include "placer/simpl/simpl_options.h"

#include <utility>

namespace ktplace {

namespace {

// The declarations, kept out of load() so the function below is only the reading.
const Option<int> kGrid{"simpl", "grid", 0};
const Option<double> kCgTol{"simpl", "cg_tol", 0.0};
const Option<std::size_t> kInitIters{"simpl", "init_iters", 7};
const Option<std::size_t> kIters{"simpl", "iters", 100};
const Option<std::size_t> kSeed{"simpl", "seed", 20240607};
const Option<std::string> kInitNet{"simpl", "init_net", "b2b"};
const Option<std::string> kLssNet{"simpl", "lss_net", "b2b"};
const Option<std::string> kPseudonet{"simpl", "pseudonet", "constant"};
const Option<std::string> kStart{"simpl", "start", "uniform"};
const Option<bool> kNoLal{"simpl", "no_lal", false};
const Option<std::size_t> kLalOnly{"simpl", "lal_only_rounds", 0};
const Option<bool> kFences{"simpl", "fences", true};
const Option<double> kDensity{"simpl", "density", 1.0};
const Option<double> kGlobalCluster{"simpl", "global_cluster", 0.0};
const Option<double> kAlphaBase{"simpl", "alpha_base", 0.01};
const Option<double> kAlphaDecay{"simpl", "alpha_decay", 0.0};
const Option<double> kAlphaDecayBelow{"simpl", "alpha_decay_below", 0.0};
const Option<std::string> kStripeScale{"simpl", "stripe_scale", ""};

}  // namespace

SimplOptions SimplOptions::load() {
    SimplOptions out;
    out.grid = kGrid.value();
    out.cgTol = kCgTol.value();
    out.initIters = kInitIters.value();
    out.iters = kIters.value();
    out.seed = kSeed.value();
    out.initNet = kInitNet.value();
    out.lssNet = kLssNet.value();
    out.pseudonet = kPseudonet.value();
    out.start = kStart.value();
    out.noLal = kNoLal.value();
    out.lalOnlyRounds = kLalOnly.value();
    out.fences = kFences.value();
    out.density = kDensity.value();
    out.globalCluster = kGlobalCluster.value();
    out.alphaBase = kAlphaBase.value();
    out.alphaDecay = kAlphaDecay.value();
    out.alphaDecayBelow = kAlphaDecayBelow.value();
    out.stripeScale = kStripeScale.value();

    // What the script actually set, which is what the placer applies.
    if (const auto configured = OptionDb::instance().optionsFor("simpl")) {
        out.configured = *configured;
    }

    // One row per declared option, so a report can only ever disagree with the
    // values above by being built from the same declarations.
    out.rows = {resolve(kGrid),
                resolve(kCgTol),
                resolve(kInitIters),
                resolve(kIters),
                resolve(kSeed),
                resolve(kInitNet),
                resolve(kLssNet),
                resolve(kPseudonet),
                resolve(kStart),
                resolve(kNoLal),
                resolve(kLalOnly),
                resolve(kFences),
                resolve(kDensity),
                resolve(kGlobalCluster),
                resolve(kAlphaBase),
                resolve(kAlphaDecay),
                resolve(kAlphaDecayBelow),
                resolve(kStripeScale)};
    return out;
}

bool SimplOptions::isSet(const char *key) const {
    return configured.find(key) != configured.end();
}

std::optional<int> SimplOptions::setInt(const char *key) const {
    const auto found = configured.find(key);
    if (found == configured.end()) {
        return std::nullopt;
    }
    return OptionTraits<int>::fromValue(found->second);
}

std::optional<double> SimplOptions::setDouble(const char *key) const {
    const auto found = configured.find(key);
    if (found == configured.end()) {
        return std::nullopt;
    }
    return OptionTraits<double>::fromValue(found->second);
}

std::optional<bool> SimplOptions::setBool(const char *key) const {
    const auto found = configured.find(key);
    if (found == configured.end()) {
        return std::nullopt;
    }
    return OptionTraits<bool>::fromValue(found->second);
}

std::optional<std::string> SimplOptions::setString(const char *key) const {
    const auto found = configured.find(key);
    if (found == configured.end()) {
        return std::nullopt;
    }
    return OptionTraits<std::string>::fromValue(found->second);
}

void SimplOptions::report() const {
    reportOptions("simpl", rows);
}

}  // namespace ktplace
