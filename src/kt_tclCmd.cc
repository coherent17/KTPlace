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

void registerStep(tcl::CommandHelp help, std::size_t minWords, std::size_t maxWords, Needs needs,
                  Step step) {
    const std::string name = help.name;
    const std::string usage = help.usage;
    tcl::TclMgr::instance().registerCommand(
        std::move(help),
        [=](tcl::Interpreter &self, const tcl::Arguments &args, tcl::Value &result) -> int {
            const std::size_t given = args.size() - 1;
            if (given < minWords || given > maxWords) {
                throw tcl::Error(std::format("wrong # args: should be \"{}\"", usage));
            }
            if (bound == nullptr) {
                throw tcl::Error(std::format("{}: no placement session is open", name));
            }
            if (needs == Needs::Design && !bound->hasDesign()) {
                throw tcl::Error(
                    std::format("{}: no design is loaded; call read_design first", name));
            }
            Words words;
            for (std::size_t i = 1; i < args.size(); ++i) {
                words.push_back(args[i].literal ? args[i].text : self.substitute(args[i].text));
            }
            try {
                result = step(*bound, words);
            } catch (const std::logic_error &error) {
                throw tcl::Error(std::format("{}: {}", name, error.what()));
            }
            return 0;
        });
}

void registerCommands() {
    registerStep(
        {.name = "read_design",
         .usage = "read_design [dir]",
         .summary = "load a Bookshelf or LEF/DEF design (default: the opened or given design)"},
        0, 1, Needs::Session, [](FlowMgr &flow, const Words &words) {
            const std::string dir = words.empty() ? flow.designPath() : words[0];
            if (dir.empty()) {
                throw std::logic_error("no design to read; give a directory or open a design");
            }
            flow.readDesign(dir);
            return tcl::Value::string(dir);
        });
    registerStep({.name = "gplace_design",
                  .usage = "gplace_design [simpl|ntuplace1]",
                  .summary = "global placement (default: the flow algorithm option)"},
                 0, 1, Needs::Design, [](FlowMgr &flow, const Words &words) {
                     if (!words.empty()) {
                         OptionDb::instance().set(Component::Flow, "algorithm",
                                                  OptionValue{words[0]});
                     }
                     flow.globalPlace();
                     return tcl::Value{};
                 });
    registerStep({.name = "legalize_placement",
                  .usage = "legalize_placement",
                  .summary = "legalize: Abacus, or the multi-row legalizer when cells span rows"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     flow.legalize();
                     return tcl::Value{};
                 });
    registerStep(
        {.name = "detail_place", .usage = "detail_place", .summary = "detailed placement (FastDP)"},
        0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
            flow.detailPlace();
            return tcl::Value{};
        });
    registerStep({.name = "place_design",
                  .usage = "place_design",
                  .summary = "gplace_design, legalize_placement and detail_place"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     flow.globalPlace();
                     flow.legalize();
                     flow.detailPlace();
                     return tcl::Value{};
                 });
    registerStep({.name = "read_macro_constraints",
                  .usage = "read_macro_constraints [mlist [txt]]",
                  .summary = "load the movable macros and their rules (default <design>.mlist, "
                             "<design>.txt in the design directory); returns the maximum "
                             "displacement in database units"},
                 0, 2, Needs::Design, [](FlowMgr &flow, const Words &words) {
                     return tcl::Value::real(flow.readMacroPlan(words.empty() ? "" : words[0],
                                                                words.size() < 2 ? "" : words[1]));
                 });
    registerStep({.name = "macro_place",
                  .usage = "macro_place",
                  .summary = "detailed macro placement: shift and flip the movable macros"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     flow.macroPlace();
                     return tcl::Value{};
                 });
    registerStep({.name = "check_macros",
                  .usage = "check_macros",
                  .summary = "check the macro rules; returns 1 if the macro placement is legal"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     return tcl::Value::boolean(flow.checkMacros());
                 });
    registerStep(
        {.name = "write_macros",
         .usage = "write_macros [file]",
         .summary =
             "write the macro placement (default <work-dir>/<design>.dmp); returns the path"},
        0, 1, Needs::Design, [](FlowMgr &flow, const Words &words) {
            return tcl::Value::string(flow.writeMacros(words.empty() ? "" : words[0]));
        });
    registerStep(
        {.name = "write_placement",
         .usage = "write_placement [file]",
         .summary = "write the placement (default <work-dir>/placed.pl); returns the path"},
        0, 1, Needs::Design, [](FlowMgr &flow, const Words &words) {
            return tcl::Value::string(flow.writePlacement(words.empty() ? "" : words[0]));
        });
    registerStep({.name = "write_plots",
                  .usage = "write_plots",
                  .summary = "draw the GIFs and the final image now"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     flow.writePlots();
                     return tcl::Value{};
                 });
    registerStep({.name = "report_hpwl",
                  .usage = "report_hpwl",
                  .summary = "log the half-perimeter wirelength and return it"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     const double hpwl = flow.hpwl();
                     ktlog.echo("HPWL {:.6e}", hpwl);
                     return tcl::Value::real(hpwl);
                 });
    registerStep({.name = "report_design",
                  .usage = "report_design",
                  .summary = "report the design's size and utilisation"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     flow.reportDesign();
                     return tcl::Value{};
                 });
    registerStep({.name = "check_placement",
                  .usage = "check_placement",
                  .summary = "check overlaps, die and fences; returns 1 if the placement is legal"},
                 0, 0, Needs::Design, [](FlowMgr &flow, const Words &) {
                     return tcl::Value::boolean(flow.checkPlacement());
                 });
}

}  // namespace

void bindTclCommands(FlowMgr &flow) {
    static bool registered = false;
    if (!registered) {
        registerCommands();
        registered = true;
    }
    bound = &flow;
}

void unbindTclCommands(const FlowMgr &flow) {
    if (bound == &flow) {
        bound = nullptr;
    }
}

}  // namespace ktplace
