// @file kt_tcl.h
// A small Tcl interpreter, enough to configure a run from a script.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace ktplace::tcl {

// A Tcl value. Tcl itself has one type; the split here is what lets a configured
// option keep the distinction between 1, 1.5 and "1.5" instead of parsing
// everything back out of a string.
class Value {
public:
    enum class Kind { Int, Double, Bool, List, String };

    Value() = default;

    [[nodiscard]] static Value integer(std::int64_t value);
    [[nodiscard]] static Value real(double value);
    [[nodiscard]] static Value boolean(bool value);
    [[nodiscard]] static Value list(std::vector<Value> items);
    [[nodiscard]] static Value string(std::string text);

    [[nodiscard]] Kind getKind() const noexcept {
        return kindOf;
    }
    [[nodiscard]] bool isInt() const noexcept {
        return kindOf == Kind::Int;
    }
    [[nodiscard]] bool isDouble() const noexcept {
        return kindOf == Kind::Double;
    }
    [[nodiscard]] bool isList() const noexcept {
        return kindOf == Kind::List;
    }

    // The conversions are total: a Tcl script may write 1 where a double is
    // wanted, and failing at the point of use gives a better message than
    // refusing to parse the script at all.
    [[nodiscard]] std::int64_t asInt() const noexcept;
    [[nodiscard]] double asDouble() const noexcept;
    [[nodiscard]] bool asBool() const noexcept;
    [[nodiscard]] const std::vector<Value> &asList() const noexcept;
    [[nodiscard]] std::string asString() const;

    // True for a number in any spelling. How a script's own quoting is told apart
    // from a deliberate string option.

    // Two values are equal when their canonical text is: that is the comparison a
    // test and a report both mean, and it makes 1 and 1.0 agree, as Tcl does.

private:
    Kind kindOf = Kind::String;
    std::int64_t intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
    std::vector<Value> listValue;
    std::string stringValue;
};

// A command failed. The interpreter stops at the first one, so a script's later
// commands do not run against state the failure left half-built.
class Error {
public:
    explicit Error(std::string text) : message(std::move(text)) {}

    [[nodiscard]] const std::string &what() const noexcept {
        return message;
    }

private:
    std::string message;
};

// One script command: the command name, then its words, with substitutions not
// yet applied. A braced word is one that must *not* be substituted, so the parser
// keeps the distinction; Value alone cannot express it.
struct Word {
    std::string text;
    bool literal = false;  // came from {...}: never substituted
};

using Arguments = std::vector<Word>;

class Interpreter;

// Returns 0 on success, or non-zero having reported the failure itself. A result,
// if the command produced one, is written to `result`.
using Command = std::function<int(Interpreter &, const Arguments &, Value &)>;

struct CommandHelp {
    std::string name;
    std::string usage;
    std::string summary;
};

class Interpreter {
public:
    Interpreter();

    // Runs a whole script and returns the last command's result. `#` comments,
    // `\` line continuations and `;` separators are handled by the parser.
    [[nodiscard]] Value eval(const std::string &script, int depth = 0);
    [[nodiscard]] Value evalFile(const std::string &path);

    void registerCommand(CommandHelp help, Command command);
    [[nodiscard]] std::string getHelpText(const std::string &name = {}) const;
    [[nodiscard]] const std::map<std::string, Command> &getCommands() const noexcept;

    // Variables, as `set` and `$name` see them.
    void setVariable(const std::string &name, Value value);
    void unsetVariable(const std::string &name);
    [[nodiscard]] Value getVariable(const std::string &name) const;
    [[nodiscard]] bool hasVariable(const std::string &name) const noexcept;

    // Replaces $var, ${var}, [command] and backslash escapes. Braced words never
    // reach it, which is what makes them literal.
    [[nodiscard]] std::string substitute(const std::string &text, int depth = 0);

    // A Tcl list literal split into its elements, honouring braces and quotes.
    [[nodiscard]] std::vector<Value> splitList(const std::string &text) const;


private:
    void installBuiltins();
    [[nodiscard]] Value run(const Arguments &args, int depth);
    [[nodiscard]] std::size_t matchBracket(const std::string &script, std::size_t open) const;
    [[nodiscard]] std::vector<std::string> parseListNames(const std::string &text) const;

    std::map<std::string, Command> commandTable;
    std::map<std::string, CommandHelp> helpTable;
    std::set<std::string> builtinNames;
    std::map<std::string, Value> variableTable;
    std::string currentCommand;
};

// Tcl's `string match`: '*' and '?' are wildcards, everything else is literal.
[[nodiscard]] bool isGlobMatch(const std::string &pattern, const std::string &text);

// True when `text` is a whole integer / a whole real number: Tcl's own rule for
// what a bare word becomes.
[[nodiscard]] bool isIntegerLiteral(const std::string &text);
[[nodiscard]] bool isDoubleLiteral(const std::string &text);

// The value a bare script word gets.
[[nodiscard]] Value parseLiteral(const std::string &text);

}  // namespace ktplace::tcl
