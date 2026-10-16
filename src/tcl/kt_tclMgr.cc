// @file kt_tclMgr.cc
// Running configuration scripts, and handing their options to the components

#include "tcl/kt_tclMgr.h"

#include "datamodel/kt_optionDb.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace ktplace::tcl {

namespace {

std::string joined(const auto &names) {
    std::string out;
    for (const std::string_view name : names) {
        out += out.empty() ? "" : ", ";
        out += name;
    }
    return out;
}

// A script word becomes the variant a component reads. The type comes from the
// word's own spelling, so 64 is an int, 1e-6 a double, and a braced {64} a string
// that happens to look like one.
OptionValue toOptionValue(const Value &value) {
    switch (value.kind()) {
        case Value::Kind::Int:
            return OptionValue{static_cast<int>(value.asInt())};
        case Value::Kind::Bool:
            return OptionValue{value.asBool()};
        case Value::Kind::Double:
            return OptionValue{value.asDouble()};
        case Value::Kind::String:
        case Value::Kind::List:
            break;
    }
    return OptionValue{value.asString()};
}

}  // namespace

TclMgr &TclMgr::instance() {
    static TclMgr manager;
    return manager;
}

TclMgr::TclMgr() {
    installCommands();
}

void TclMgr::installCommands() {
    interpreter.registerCommand(
        "add_option", [](tcl::Interpreter &self, const Arguments &args, Value &) -> int {
            if (args.size() != 4) {
                throw tcl::Error("add_option: expected <component> <key> <value>");
            }
            const std::string component = self.substitute(args[1].text);
            const std::string key = self.substitute(args[2].text);
            if (component.empty() || key.empty()) {
                throw tcl::Error("add_option: the component and the key must not be empty");
            }
            const std::optional<Component> id = componentFromName(component);
            if (!id) {
                throw tcl::Error(
                    std::format("add_option: '{}' is not a known component (try one of: {})",
                                component, joined(componentNames)));
            }
            const Value value = args[3].literal ? Value::string(args[3].text)
                                                : parseLiteral(self.substitute(args[3].text));
            OptionDb::instance().set(*id, key, toOptionValue(value));
            return 0;
        });
}

void TclMgr::registerCommand(const std::string &name, tcl::Command command) {
    interpreter.registerCommand(name, std::move(command));
}

// A malformed script raises rather than exiting: whether a bad script is fatal
// is the caller's decision, and a caller that wants a report instead needs the
// error to come back rather than the process to end.
void TclMgr::loadScript(const std::string &path) {
    (void)interpreter.evalFile(path);
    OptionDb::instance().report();
}

}  // namespace ktplace::tcl
