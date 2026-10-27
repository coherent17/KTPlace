#include "kt_tclCmd.h"

#include "kt_flowMgr.h"

#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tcl.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"

#include <cstddef>
#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

FlowMgr *bound = nullptr;

using Words = std::vector<std::string>;
using Step = std::function<tcl::Value(FlowMgr &, const Words &)>;

enum class Needs { Session, Design };

struct Arity {
    std::size_t min = 0;
    std::size_t max = 0;
};

Step getAction(auto method) {
    return [method](FlowMgr &flow, const Words &) {
        std::invoke(method, flow);
        return tcl::Value{};
    };
}

Words getSubstitutedWords(tcl::Interpreter &interp, const tcl::Arguments &args) {
    Words words;
    words.reserve(args.size() - 1);
    for (std::size_t i = 1; i < args.size(); ++i) {
        words.push_back(args[i].literal ? args[i].text : interp.substitute(args[i].text));
    }
    return words;
}

void registerStep(tcl::CommandHelp help, Arity arity, Needs needs, Step step) {
    tcl::TclMgr::getInstance().registerCommand(
        help, [help, arity, needs, step = std::move(step)](
                  tcl::Interpreter &interp, const tcl::Arguments &args, tcl::Value &result) {
            const std::size_t given = args.size() - 1;
            if (given < arity.min || given > arity.max) {
                throw tcl::Error(std::format("wrong # args: should be \"{}\"", help.usage));
            }
            if (bound == nullptr) {
                throw tcl::Error(std::format("{}: no placement session is open", help.name));
            }
            if (needs == Needs::Design && !bound->hasDesign()) {
                throw tcl::Error(
                    std::format("{}: no design is loaded; call read_design first", help.name));
            }
            try {
                result = step(*bound, getSubstitutedWords(interp, args));
            } catch (const std::logic_error &error) {
                throw tcl::Error(std::format("{}: {}", help.name, error.what()));
            }
            return 0;
        });
}

std::string getFirstOr(const Words &words, std::string fallback) {
    return words.empty() ? std::move(fallback) : words.front();
}

void registerCommands() {
    registerStep(
        {.name = "read_design",
         .usage = "read_design [dir]",
         .summary = "load a Bookshelf or LEF/DEF design (default: the opened or given design)"},
        {.min = 0, .max = 1}, Needs::Session, [](FlowMgr &flow, const Words &words) {
            const std::string dir = getFirstOr(words, flow.getDesignPath());
            if (dir.empty()) {
                throw std::logic_error("no design to read; give a directory or open a design");
            }
            flow.readDesign(dir);
            return tcl::Value::string(dir);
        });
    registerStep({.name = "gplace_design",
                  .usage = "gplace_design [simpl|ntuplace1|kraftwerk2]",
                  .summary = "global placement (default: the flow algorithm option)"},
                 {.min = 0, .max = 1}, Needs::Design, [](FlowMgr &flow, const Words &words) {
                     if (!words.empty()) {
                         OptionDb::getInstance().set(Component::Flow, "algorithm",
                                                     OptionValue{words.front()});
                     }
                     flow.globalPlace();
                     return tcl::Value{};
                 });
    registerStep({.name = "legalize_placement",
                  .usage = "legalize_placement",
                  .summary = "legalize: Abacus, or the multi-row legalizer when cells span rows"},
                 {}, Needs::Design, getAction(&FlowMgr::legalize));
    registerStep(
        {.name = "detail_place", .usage = "detail_place", .summary = "detailed placement (FastDP)"},
        {}, Needs::Design, getAction(&FlowMgr::detailPlace));
    registerStep({.name = "place_design",
                  .usage = "place_design",
                  .summary = "gplace_design, legalize_placement and detail_place"},
                 {}, Needs::Design, getAction([](FlowMgr &flow) {
                     flow.globalPlace();
                     flow.legalize();
                     flow.detailPlace();
                 }));
    registerStep(
        {.name = "write_placement",
         .usage = "write_placement [file]",
         .summary = "write the placement (default <work-dir>/placed.pl); returns the path"},
        {.min = 0, .max = 1}, Needs::Design, [](FlowMgr &flow, const Words &words) {
            return tcl::Value::string(flow.writePlacement(getFirstOr(words, "")));
        });
    registerStep({.name = "write_plots",
                  .usage = "write_plots",
                  .summary = "draw the GIFs and the final image now"},
                 {}, Needs::Design, getAction(&FlowMgr::writePlots));
    registerStep({.name = "report_hpwl",
                  .usage = "report_hpwl",
                  .summary = "log the half-perimeter wirelength and return it"},
                 {}, Needs::Design, [](FlowMgr &flow, const Words &) {
                     const double hpwl = flow.getHpwl();
                     ktlog.echo("HPWL {:.6e}", hpwl);
                     return tcl::Value::real(hpwl);
                 });
    registerStep({.name = "report_design",
                  .usage = "report_design",
                  .summary = "report the design's size and utilisation"},
                 {}, Needs::Design, getAction(&FlowMgr::reportDesign));
    registerStep({.name = "check_placement",
                  .usage = "check_placement",
                  .summary = "check overlaps, die and fences; returns 1 if the placement is legal"},
                 {}, Needs::Design, [](FlowMgr &flow, const Words &) {
                     return tcl::Value::boolean(flow.isPlacementLegal());
                 });
}

}  // namespace

void bindTclCommands(FlowMgr &flow) {
    static const bool registered = (registerCommands(), true);
    (void)registered;
    bound = &flow;
}

void unbindTclCommands(const FlowMgr &flow) {
    if (bound == &flow) {
        bound = nullptr;
    }
}

}  // namespace ktplace
