#include "kt_tclCommand.h"

#include "kt_flowMgr.h"

#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tcl.h"
#include "tcl/kt_tclMgr.h"
#include "util/kt_log.h"

#include <algorithm>
#include <format>
#include <functional>
#include <stdexcept>

namespace ktplace {

namespace {

FlowMgr *bound = nullptr;

using Words = std::vector<std::string>;
using Handler = std::function<tcl::Value(FlowMgr &, const Words &)>;

struct TclCommand {
    CommandHelp help;
    std::size_t minWords = 0;
    std::size_t maxWords = 0;
    bool needsDesign = true;
    Handler run;
};

const std::vector<TclCommand> &tclCommands() {
    static const std::vector<TclCommand> table{
        {{"read_design", "read_design <dir>", "load a Bookshelf or LEF/DEF design directory"},
         1,
         1,
         false,
         [](FlowMgr &flow, const Words &words) {
             flow.readDesign(words[0]);
             return tcl::Value{};
         }},
        {{"gplace_design", "gplace_design [simpl|ntuplace1]",
          "global placement (default: the flow algorithm option)"},
         0,
         1,
         true,
         [](FlowMgr &flow, const Words &words) {
             if (!words.empty()) {
                 OptionDb::instance().set(Component::Flow, "algorithm", OptionValue{words[0]});
             }
             flow.globalPlace();
             return tcl::Value{};
         }},
        {{"legalize_placement", "legalize_placement",
          "legalize: Abacus, or the multi-row legalizer when cells span rows"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             flow.legalize();
             return tcl::Value{};
         }},
        {{"detail_place", "detail_place", "detailed placement (FastDP)"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             flow.detailPlace();
             return tcl::Value{};
         }},
        {{"place_design", "place_design", "gplace_design, legalize_placement and detail_place"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             flow.globalPlace();
             flow.legalize();
             flow.detailPlace();
             return tcl::Value{};
         }},
        {{"write_placement", "write_placement [file]",
          "write the placement (default <work-dir>/placed.pl); returns the path"},
         0,
         1,
         true,
         [](FlowMgr &flow, const Words &words) {
             return tcl::Value::string(flow.writePlacement(words.empty() ? "" : words[0]));
         }},
        {{"write_plots", "write_plots", "draw the GIFs and the final image now"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             flow.writePlots();
             return tcl::Value{};
         }},
        {{"report_hpwl", "report_hpwl", "log the half-perimeter wirelength and return it"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             const double hpwl = flow.hpwl();
             ktlog.echo("HPWL {:.6e}", hpwl);
             return tcl::Value::real(hpwl);
         }},
        {{"report_design", "report_design", "report the design's size and utilisation"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             flow.reportDesign();
             return tcl::Value{};
         }},
        {{"check_placement", "check_placement",
          "check overlaps, die and fences; returns 1 if the placement is legal"},
         0,
         0,
         true,
         [](FlowMgr &flow, const Words &) {
             return tcl::Value::boolean(flow.checkPlacement());
         }},
    };
    return table;
}

const std::vector<CommandHelp> &otherCommands() {
    static const std::vector<CommandHelp> others{
        {"add_option", "add_option <component> <key> <value>",
         "set an option, e.g. add_option simpl grid 64"},
        {"help", "help [command]", "list the commands, or show one command's usage"},
        {"source", "source <file>", "run a Tcl script"},
        {"puts", "puts <text>", "print to the log"},
    };
    return others;
}

const char *tclBuiltins =
    "set unset expr if while foreach incr list lappend lindex llength string info error";

tcl::Value invoke(const TclCommand &command, tcl::Interpreter &self, const tcl::Arguments &args) {
    const std::size_t given = args.size() - 1;
    if (given < command.minWords || given > command.maxWords) {
        throw tcl::Error(std::format("wrong # args: should be \"{}\"", command.help.usage));
    }
    if (bound == nullptr) {
        throw tcl::Error(std::format("{}: no placement session is open", command.help.name));
    }
    if (command.needsDesign && !bound->hasDesign()) {
        throw tcl::Error(
            std::format("{}: no design is loaded; call read_design first", command.help.name));
    }
    Words words;
    for (std::size_t i = 1; i < args.size(); ++i) {
        words.push_back(args[i].literal ? args[i].text : self.substitute(args[i].text));
    }
    try {
        return command.run(*bound, words);
    } catch (const std::logic_error &error) {
        throw tcl::Error(std::format("{}: {}", command.help.name, error.what()));
    }
}

void registerCommands() {
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    tcl::TclMgr &tcl = tcl::TclMgr::instance();
    for (const TclCommand &command : tclCommands()) {
        tcl.registerCommand(command.help.name,
                            [&command](tcl::Interpreter &self, const tcl::Arguments &args,
                                       tcl::Value &result) -> int {
                                result = invoke(command, self, args);
                                return 0;
                            });
    }
    tcl.registerCommand(
        "help", [](tcl::Interpreter &self, const tcl::Arguments &args, tcl::Value &result) -> int {
            if (args.size() > 2) {
                throw tcl::Error("wrong # args: should be \"help [command]\"");
            }
            result =
                tcl::Value::string(helpText(args.size() == 2 ? self.substitute(args[1].text) : ""));
            return 0;
        });
}

}  // namespace

void bindTclCommands(FlowMgr &flow) {
    registerCommands();
    bound = &flow;
}

void unbindTclCommands(const FlowMgr &flow) {
    if (bound == &flow) {
        bound = nullptr;
    }
}

const std::vector<CommandHelp> &commandHelp() {
    static const std::vector<CommandHelp> all = [] {
        std::vector<CommandHelp> help;
        for (const TclCommand &command : tclCommands()) {
            help.push_back(command.help);
        }
        const auto &others = otherCommands();
        help.insert(help.end(), others.begin(), others.end());
        return help;
    }();
    return all;
}

std::string helpText(const std::string &command) {
    const std::vector<CommandHelp> &help = commandHelp();
    if (!command.empty()) {
        const auto found = std::ranges::find(help, command, &CommandHelp::name);
        if (found == help.end()) {
            throw tcl::Error(std::format("help: no command \"{}\"; try help", command));
        }
        return std::format("{}\n  {}", found->usage, found->summary);
    }
    std::size_t width = 0;
    for (const CommandHelp &entry : help) {
        width = std::max(width, entry.usage.size());
    }
    std::string text = "Commands:";
    for (const CommandHelp &entry : help) {
        text += std::format("\n  {:<{}}  {}", entry.usage, width, entry.summary);
    }
    text += std::format("\nTcl builtins: {}", tclBuiltins);
    text += "\nhelp <command> shows one command; Up/Down recall earlier commands.";
    return text;
}

}  // namespace ktplace
