// @file kt_tclShell.h
// A Tcl interpreter bound to one ktSession: the command layer every front end
// drives the engine through.
//
// The batch mode (`ktplace -t script.tcl`) and the GUI's console both evaluate
// Tcl here, and a GUI action is meant to be issued as the same command a user
// could type. So a GUI session replays as a script, and everything the GUI can
// do is reachable, and testable, without a display.
//
// Commands map one-to-one onto ktSession's stages and the parameter registry;
// see registerCommands() in kt_tclShell.cc for the list, or `help` in the shell.
// A command that fails -- a bad argument, or a ktlog.fatal inside a stage --
// returns TCL_ERROR with the message, so a script can `catch` it and an
// interactive session carries on. Nothing here exits the process.
//
// Tcl's own `puts` to stdout or stderr is rerouted to the log: stdout is never
// written to (see util/kt_log.h). `puts` to a file channel is untouched.
//
// Each top-level command is echoed to the log as "tcl> <command>" before it
// runs, so ktplace.log reads as the script that produced it.
//
// One shell per thread: a Tcl interpreter may only be used from the thread that
// created it.

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ktplace {

class ktSession;

class TclShell {
public:
    /// The outcome of one evaluation.
    struct Result {
        bool ok = true;
        /// The command's result, or on error its message.
        std::string text;
        /// On error, Tcl's errorInfo: the message plus where it was raised
        /// (command, procedure, file and line). Empty on success.
        std::string trace;
    };

    /// Binds a new interpreter to @p session, which must outlive the shell.
    explicit TclShell(ktSession &session);
    ~TclShell();

    TclShell(const TclShell &) = delete;
    TclShell &operator=(const TclShell &) = delete;

    /// Evaluate @p script at global level.
    Result eval(std::string_view script);

    /// Evaluate the file at @p path, as `source` would.
    Result evalFile(const std::string &path);

    /// Set element @p key of the global array `ktplace`, which is how the
    /// command line hands a script its work directory and input.
    void setGlobal(std::string_view key, std::string_view value);

    /// Whether @p script is a complete command, so a console knows whether to
    /// run what was typed or to read another line (an open brace, say).
    [[nodiscard]] static bool isComplete(std::string_view script);

    /// The commands this shell adds to Tcl, sorted, for help and completion.
    [[nodiscard]] std::vector<std::string> commandNames() const;

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
