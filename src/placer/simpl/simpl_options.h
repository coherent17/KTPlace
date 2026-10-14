// @file simpl_options.h
// SimPL's options, and where they came from

#pragma once

#include "datamodel/kt_optionDb.h"

#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

// Every option SimPL reads, in one place.
//
// The keys are what a Tcl script writes:
//
//     add_option simpl grid 64
//     add_option simpl cg_tol 1e-6
//
// Each was a KTPLACE_SIMPL_* environment variable. The environment is no longer
// read: an option the script did not set takes the default below.
struct SimplOptions {
    // Grid resolution. Zero means "derive from the cell count", which is what the
    // placer does when this is not set: sqrt(movable / 51) clamped to [16, 256].
    int grid = 0;

    // Stop the conjugate-gradient solve once the residual is this small. The solve
    // is bounded by its iteration count in practice, so this rarely ends a solve
    // on its own.
    double cgTol = 0.0;

    std::size_t initIters = 7;
    std::size_t iters = 100;
    // The pseudo-random stream the start point is drawn from.
    std::size_t seed = 20240607;

    // Net models, by name: init_net and lss_net choose the B2B or clustered net
    // model for the two stages; pseudonet chooses the cluster-law used when the
    // cells that net would connect have no fixed positions.
    std::string initNet = "b2b";
    std::string lssNet = "b2b";
    std::string pseudonet = "constant";

    // Where the solver starts: "input", "auto" or "uniform". Uniform spreads the
    // cells evenly first, which is the paper's seed; input starts from the
    // placement the design shipped with.
    std::string start = "uniform";

    // Look-ahead legalization. no_lal and lal_only both turn parts of it off, and
    // fences is whether placement regions are honoured at all.
    bool noLal = false;
    // Look-ahead only: how many rounds of the spread projection to run, holding
    // the input fixed. Zero means run the full solve as usual.
    std::size_t lalOnlyRounds = 0;
    bool fences = true;

    double density = 1.0;
    double globalCluster = 0.0;
    double alphaBase = 0.01;
    double alphaDecay = 0.0;
    double alphaDecayBelow = 0.0;

    // "tight", "room" or empty for the standard divisor. See the paper's Figure 5.
    std::string stripeScale = "";

    // How often the solver offers a snapshot, which is SimPL's to say because it
    // knows how long its loops are. Drawing them is the host's business, and with
    // no plots nothing is built whatever these say.
    //   trace_every    bounds every N global iterations (0 = none)
    //   cg_every       every N CG iterations inside a global solve (0 = none)
    //   cg_every_init  the same inside the warm-up solves; when not set, the flow
    //                  turns these off unless the run is animating
    //   density_maps   a bin-density map beside every placement frame
    std::size_t traceEvery = 1;
    std::size_t cgEvery = 0;
    std::size_t cgEveryInit = 1;
    bool densityMaps = true;

    // Reads every option, and records each one's value, default and origin for
    // the report. Call once, before the solver runs.
    [[nodiscard]] static SimplOptions load();

    // True when a script set this option, as opposed to the default standing. A
    // component applies an option only when this is true, so an option's default
    // here documents the run rather than overriding the component's own default
    // with a second copy of it.
    [[nodiscard]] bool isSet(const char *key) const;

    // The option's value when a script set it, and nothing otherwise. Use
    // `if (auto v = set("grid")) { ... }` to apply an option without disturbing the
    // component's default when it was not asked for.
    [[nodiscard]] std::optional<int> setInt(const char *key) const;
    [[nodiscard]] std::optional<double> setDouble(const char *key) const;
    [[nodiscard]] std::optional<bool> setBool(const char *key) const;
    [[nodiscard]] std::optional<std::string> setString(const char *key) const;

    // Prints the table: key, value, default, and where the value came from.
    void report() const;

    [[nodiscard]] const std::vector<ResolvedOption> &resolved() const noexcept {
        return rows;
    }

private:
    std::vector<ResolvedOption> rows;
    std::unordered_map<std::string, OptionValue> configured;
};

}  // namespace ktplace
