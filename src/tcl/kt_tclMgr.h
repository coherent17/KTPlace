// @file kt_tclMgr.h
// Running configuration scripts, and handing their options to the components.

#pragma once

#include "datamodel/kt_optionDb.h"
#include "tcl/kt_tcl.h"

#include <string>

namespace ktplace::tcl {

// Owns the interpreter a run is configured with, and moves what a script sets into
// the option store the components read.
//
// One command is supported, `add_option`:
//
//     add_option simpl grid 64
//     add_option simpl cg_tol 1e-6
//     add_option simpl fences 0
//     add_option abacus max_row_dist 3
//     set g 128
//     add_option simpl grid $g
//
// The value is typed by its spelling, exactly as Tcl types a bare word, so a
// component can ask for the type it expects with ComponentOptions::get<T>. Other commands are
// registered alongside; the interpreter is not restricted to this one.
class TclMgr {
public:
    // The run's manager. Process-wide because there is one script and one set of
    // options for the whole process.
    static TclMgr &getInstance();

    TclMgr();

    void loadScript(const std::string &path);

    // Registers a command, so the set a script may call grows without touching
    // this class.
    void registerCommand(tcl::CommandHelp help, tcl::Command command);

    // The interpreter the script runs in. Public because registering more
    // commands is the whole extension point, and a caller may reasonably want to
    // read a variable the script left behind.
    tcl::Interpreter interpreter;

    // What the store holds, for a report or a test.
    [[nodiscard]] const OptionDb &getDb() const noexcept {
        return OptionDb::getInstance();
    }

    [[nodiscard]] std::size_t getOptionCount() const noexcept {
        return OptionDb::getInstance().size();
    }

private:
    // Every command a script may call without the run asking for it. One method
    // so adding a command is one entry here rather than a constructor edit.
    void installCommands();
};

}  // namespace ktplace::tcl
