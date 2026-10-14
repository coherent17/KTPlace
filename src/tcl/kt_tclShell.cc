// @file kt_tclShell.cc
// Implementation of TclShell: the interpreter, the command table, and the two
// pieces of plumbing that keep Tcl inside the logging rules (the command echo
// and the rerouted `puts`).

#include "tcl/kt_tclShell.h"

#include "datamodel/kt_optionDb.h"
#include "session/kt_session.h"
#include "tcl/kt_tcl.h"
#include "util/kt_log.h"
#include "util/kt_progress.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_plotOptions.h"

#include <algorithm>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tcl.h>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ktplace {

namespace {

// A command's own argument check failing. Reported as the command's TCL_ERROR
// result, like a fatal raised inside a stage.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The words after the command name.
using Words = std::vector<std::string>;

struct Command {
    std::string name;
    std::string synopsis;  // "read_design <dir>"
    std::string summary;
    // The command's result, or nullptr for an empty one.
    std::function<Tcl_Obj *(const Command &, const Words &)> run;
};

Tcl_Obj *newString(std::string_view s) {
    return Tcl_NewStringObj(s.data(), static_cast<int>(s.size()));
}

void expectWords(const Command &cmd, const Words &words, std::size_t min, std::size_t max) {
    if (words.size() < min || words.size() > max) {
        throw UsageError("wrong # args: should be \"" + cmd.synopsis + "\"");
    }
}

std::string_view trimTrailing(std::string_view s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' ||
                          s.back() == '\t' || s.back() == ';')) {
        s.remove_suffix(1);
    }
    return s;
}

// Passed to Tcl_CreateObjTrace to trace commands at any nesting depth.
constexpr int kTraceAllLevels = 1 << 20;

// Tcl wants the executable's location once per process before the first
// interpreter, to find its encodings and init scripts.
void initTclLibrary() {
    static std::once_flag once;
    std::call_once(once, [] {
        Tcl_FindExecutable(nullptr);
    });
}

}  // namespace

class TclShell::Impl {
public:
    explicit Impl(ktSession &s);
    ~Impl();

    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    Result finish(int code);

    ktSession &session;
    Tcl_Interp *interp = nullptr;
    std::vector<std::unique_ptr<Command>> commands;

    // The command echo. Commands at or below echoLevel are echoed: 1 is the
    // script being evaluated; each `source` from an echoed command raises it by
    // one for the file's duration, so a sourced script is in the log too. The
    // bodies of procedures stay out at any depth. lastLevel is the level of the
    // command most recently traced -- inside `source`, that is the source itself.
    int echoLevel = 1;
    int lastLevel = 0;

private:
    using Fn = std::function<Tcl_Obj *(const Command &, const Words &)>;

    void add(std::string name, std::string synopsis, std::string summary, Fn run);
    void registerCommands();
    void reroutePuts();
    void rerouteSource();

    static int dispatch(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
    static int puts(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
    static int source(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]);
    static int echo(ClientData data, Tcl_Interp *interp, int level, const char *command,
                    Tcl_Command info, int objc, Tcl_Obj *const objv[]);
};

TclShell::Impl::Impl(ktSession &s) : session(s) {
    initTclLibrary();
    interp = Tcl_CreateInterp();
    // Tcl_Init sources init.tcl, which provides the library procedures (parray,
    // auto-loading, clock). The core commands work without it, so a missing
    // library is worth a trace line, not a failure.
    if (Tcl_Init(interp) != TCL_OK) {
        ktlog.trace("Tcl library not initialised: {}", Tcl_GetStringResult(interp));
        Tcl_ResetResult(interp);
    }
    registerCommands();
    reroutePuts();
    rerouteSource();
    // Every level is traced and echo() picks what to show (see echoLevel), so
    // the log reads like the script, sourced files included.
    Tcl_CreateObjTrace(interp, kTraceAllLevels, 0, &Impl::echo, this, nullptr);
}

TclShell::Impl::~Impl() {
    Tcl_DeleteInterp(interp);
}

void TclShell::Impl::add(std::string name, std::string synopsis, std::string summary, Fn run) {
    auto cmd = std::make_unique<Command>(
        Command{std::move(name), std::move(synopsis), std::move(summary), std::move(run)});
    Tcl_CreateObjCommand(interp, cmd->name.c_str(), &Impl::dispatch, cmd.get(), nullptr);
    commands.push_back(std::move(cmd));
}

int TclShell::Impl::dispatch(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    const Command &cmd = *static_cast<const Command *>(data);
    Words words;
    words.reserve(static_cast<std::size_t>(objc));
    for (int i = 1; i < objc; ++i) {
        words.emplace_back(Tcl_GetString(objv[i]));
    }
    // Nothing may unwind through Tcl's C frames, so every exception stops here
    // and becomes the command's error. A FatalError has already been logged
    // where it was raised; its message is all the caller needs.
    std::string error;
    try {
        // A stop ends the stage that is running (it returns what it has) and
        // then the script: every KTPlace command after it fails, so a flow
        // does not go on to legalize a placement the user asked to abandon.
        // The host clears the request before its next script.
        if (Progress::instance().stopRequested()) {
            throw UsageError(cmd.name + ": stopped on request");
        }
        if (Tcl_Obj *result = cmd.run(cmd, words)) {
            Tcl_SetObjResult(interp, result);
        } else {
            Tcl_ResetResult(interp);
        }
        return TCL_OK;
    } catch (const FatalError &e) {
        error = e.message();
    } catch (const std::exception &e) {
        error = e.what();
    } catch (...) {
        error = cmd.name + ": unknown error";
    }
    Tcl_SetObjResult(interp, newString(error));
    return TCL_ERROR;
}

int TclShell::Impl::echo(ClientData data, Tcl_Interp *, int level, const char *command, Tcl_Command,
                         int, Tcl_Obj *const[]) {
    auto &self = *static_cast<Impl *>(data);
    self.lastLevel = level;
    const std::string_view text = trimTrailing(command);
    // The renamed originals are called by the reroutes, never typed.
    if (level <= self.echoLevel && !text.starts_with("::tcl::kt_")) {
        ktlog.echo("tcl> {}", text);
    }
    return TCL_OK;
}

void TclShell::Impl::rerouteSource() {
    if (Tcl_Eval(interp, "rename ::source ::tcl::kt_source") != TCL_OK) {
        throw std::logic_error(std::string("cannot reroute source: ") +
                               Tcl_GetStringResult(interp));
    }
    Tcl_CreateObjCommand(interp, "::source", &Impl::source, this, nullptr);
}

// source fileName
//
// Tcl_EvalFile, as Tcl's own source does, with the echo level raised for the
// file when the source itself was echoed. The other forms (-encoding) go to the
// original unchanged.
int TclShell::Impl::source(ClientData data, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    auto &self = *static_cast<Impl *>(data);
    if (objc != 2) {
        std::vector<Tcl_Obj *> args(objv, objv + objc);
        args[0] = Tcl_NewStringObj("::tcl::kt_source", -1);
        Tcl_IncrRefCount(args[0]);
        const int code = Tcl_EvalObjv(interp, objc, args.data(), 0);
        Tcl_DecrRefCount(args[0]);
        return code;
    }
    const int saved = self.echoLevel;
    if (self.lastLevel <= self.echoLevel) {
        self.echoLevel = self.lastLevel + 1;
    }
    const int code = Tcl_EvalFile(interp, Tcl_GetString(objv[1]));
    self.echoLevel = saved;
    return code;
}

void TclShell::Impl::reroutePuts() {
    if (Tcl_Eval(interp, "rename ::puts ::tcl::kt_puts") != TCL_OK) {
        throw std::logic_error(std::string("cannot reroute puts: ") + Tcl_GetStringResult(interp));
    }
    Tcl_CreateObjCommand(interp, "::puts", &Impl::puts, nullptr, nullptr);
}

// puts ?-nonewline? ?channelId? string
//
// stdout and stderr go to the log as one record; any other channel is Tcl's.
// -nonewline is meaningless for a log record, which is always a whole line.
int TclShell::Impl::puts(ClientData, Tcl_Interp *interp, int objc, Tcl_Obj *const objv[]) {
    int first = 1;
    if (objc > 2 && std::string_view(Tcl_GetString(objv[1])) == "-nonewline") {
        first = 2;
    }
    const int rest = objc - first;
    if (rest < 1 || rest > 2) {
        Tcl_WrongNumArgs(interp, 1, objv, "?-nonewline? ?channelId? string");
        return TCL_ERROR;
    }
    const std::string_view channel = rest == 2 ? Tcl_GetString(objv[first]) : "stdout";
    if (channel == "stdout" || channel == "stderr") {
        ktlog.echo(std::string_view(Tcl_GetString(objv[objc - 1])));
        Tcl_ResetResult(interp);
        return TCL_OK;
    }
    std::vector<Tcl_Obj *> args(objv, objv + objc);
    args[0] = Tcl_NewStringObj("::tcl::kt_puts", -1);
    Tcl_IncrRefCount(args[0]);
    const int code = Tcl_EvalObjv(interp, objc, args.data(), 0);
    Tcl_DecrRefCount(args[0]);
    return code;
}

void TclShell::Impl::registerCommands() {
    // --- the design ----------------------------------------------------------
    add("read_design", "read_design <dir>",
        "load the design in <dir>, replacing any design already loaded",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 1, 1);
            session.readDesign(w[0]);
            return nullptr;
        });
    add("report_design", "report_design", "log the loaded design's statistics",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            session.reportDesign();
            return nullptr;
        });
    add("write_design", "write_design <path>", "write the current placement as a Bookshelf .pl",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 1, 1);
            session.writeDesign(w[0]);
            return nullptr;
        });

    // --- the stages ----------------------------------------------------------
    add("global_place", "global_place ?-algo simpl|ntuplace1?",
        "run global placement (default algorithm: simpl)",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            std::string algorithm = "simpl";
            if (!w.empty()) {
                expectWords(c, w, 2, 2);
                if (w[0] != "-algo") {
                    throw UsageError("global_place: unknown option \"" + w[0] + "\"; should be \"" +
                                     c.synopsis + "\"");
                }
                algorithm = w[1];
            }
            session.globalPlace(algorithm);
            return nullptr;
        });
    add("legalize", "legalize", "legalize onto the rows (Abacus, or multi-row for tall cells)",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            session.legalize();
            return nullptr;
        });
    add("detail_place", "detail_place", "run FastDP detailed placement",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            session.detailPlace();
            return nullptr;
        });

    // --- queries -------------------------------------------------------------
    add("report_hpwl", "report_hpwl", "log and return the current half-perimeter wirelength",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            const double hpwl = session.hpwl();
            ktlog.echo("HPWL: {:.6e}", hpwl);
            return Tcl_NewDoubleObj(hpwl);
        });
    add("check_placement", "check_placement ?label?",
        "log the legality check; return its defects as {what count} pairs (empty = legal)",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 1);
            const auto defects = session.reportPlacementCheck(w.empty() ? "check_placement" : w[0]);
            Tcl_Obj *list = Tcl_NewListObj(0, nullptr);
            for (const ktDM::Defect &d : defects) {
                Tcl_Obj *pair[2] = {newString(d.what),
                                    Tcl_NewWideIntObj(static_cast<Tcl_WideInt>(d.count))};
                Tcl_ListObjAppendElement(nullptr, list, Tcl_NewListObj(2, pair));
            }
            return list;
        });
    add("report_timing", "report_timing", "log the time spent in each stage so far",
        [](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            TimerRegistry::instance().report();
            return nullptr;
        });

    // --- pictures ------------------------------------------------------------
    add("set_plot_dir", "set_plot_dir <dir>",
        "draw into <dir> (\"\" draws nothing); the visualizer options are read now",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 1, 1);
            PlotOptions plot = w[0].empty() ? PlotOptions{} : PlotOptions::fromOptions();
            plot.dir = w[0];
            session.setPlotOptions(plot);
            return nullptr;
        });
    add("write_final_image", "write_final_image", "draw the current placement as final.png",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            session.renderFinalImage();
            return nullptr;
        });
    add("write_animation", "write_animation",
        "assemble the recorded frames into placement.gif; return whether one was written",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            return Tcl_NewBooleanObj(session.finishAnimation() ? 1 : 0);
        });

    // --- options -------------------------------------------------------------
    //
    // The option store the stages read (datamodel/kt_optionDb.h). add_option is
    // the configuration interpreter's command (tcl/kt_tclMgr.h) with the same
    // typing, so one configuration script runs in either: 64 is an int, 1e-6 a
    // double, true/false a bool, anything else a string.
    add("add_option", "add_option <component> <key> <value>",
        "set an option for a stage (simpl, ntuplace1, abacus, fastdp, visualizer)",
        [](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 3, 3);
            if (w[0].empty() || w[1].empty()) {
                throw UsageError("add_option: the component and the key must not be empty");
            }
            const tcl::Value v = tcl::parseLiteral(w[2]);
            OptionValue value = v.asString();
            if (v.isInt()) {
                value = static_cast<int>(v.asInt());
            } else if (v.isDouble()) {
                value = v.asDouble();
            } else if (v.isBool()) {
                value = v.asBool();
            }
            OptionDb::instance().set(w[0], w[1], std::move(value), OptionOrigin::Script);
            return nullptr;
        });
    add("get_option", "get_option <component> <key>",
        "return an option's value as a stage will read it, or \"\" when nothing sets it",
        [](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 2, 2);
            const auto value = OptionDb::instance().find(w[0], w[1]);
            if (!value) {
                return newString("");
            }
            return std::visit(
                [](const auto &v) -> Tcl_Obj * {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, int>) {
                        return Tcl_NewIntObj(v);
                    } else if constexpr (std::is_same_v<T, bool>) {
                        return Tcl_NewBooleanObj(v ? 1 : 0);
                    } else if constexpr (std::is_same_v<T, double>) {
                        return Tcl_NewDoubleObj(v);
                    } else {
                        return newString(v);
                    }
                },
                *value);
        });
    add("report_options", "report_options",
        "log the options scripts have set (each stage reports its full table when it runs)",
        [](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 0);
            OptionDb::instance().report();
            return nullptr;
        });

    // --- help ----------------------------------------------------------------
    add("help", "help ?command?", "list the KTPlace commands, or describe one",
        [this](const Command &c, const Words &w) -> Tcl_Obj * {
            expectWords(c, w, 0, 1);
            ktReportTable table("KTPlace commands");
            table.setHeaders({"command", "description"});
            bool found = false;
            for (const auto &cmd : commands) {
                if (!w.empty() && cmd->name != w[0]) {
                    continue;
                }
                table.addRow({cmd->synopsis, cmd->summary});
                found = true;
            }
            if (!found) {
                throw UsageError("help: no KTPlace command \"" + w[0] + "\"");
            }
            table.emit();
            return nullptr;
        });
}

TclShell::Result TclShell::Impl::finish(int code) {
    Result r;
    // `return` at the top of a script ends it normally, as it does for tclsh.
    r.ok = code == TCL_OK || code == TCL_RETURN;
    r.text = Tcl_GetStringResult(interp);
    if (!r.ok) {
        const char *info = Tcl_GetVar(interp, "errorInfo", TCL_GLOBAL_ONLY);
        r.trace = info != nullptr ? info : r.text;
    }
    return r;
}

TclShell::TclShell(ktSession &session) : pImpl(std::make_unique<Impl>(session)) {}
TclShell::~TclShell() = default;

TclShell::Result TclShell::eval(std::string_view script) {
    const int code =
        Tcl_EvalEx(pImpl->interp, script.data(), static_cast<int>(script.size()), TCL_EVAL_GLOBAL);
    return pImpl->finish(code);
}

TclShell::Result TclShell::evalFile(const std::string &path) {
    return pImpl->finish(Tcl_EvalFile(pImpl->interp, path.c_str()));
}

void TclShell::setGlobal(std::string_view key, std::string_view value) {
    Tcl_SetVar2(pImpl->interp, "ktplace", std::string(key).c_str(), std::string(value).c_str(),
                TCL_GLOBAL_ONLY);
}

bool TclShell::isComplete(std::string_view script) {
    initTclLibrary();
    return Tcl_CommandComplete(std::string(script).c_str()) != 0;
}

std::vector<std::string> TclShell::commandNames() const {
    std::vector<std::string> names;
    names.reserve(pImpl->commands.size());
    for (const auto &cmd : pImpl->commands) {
        names.push_back(cmd->name);
    }
    std::ranges::sort(names);
    return names;
}

}  // namespace ktplace
