// @file kt_tcl.cc
// Implementation of the small Tcl interpreter

#include "tcl/kt_tcl.h"

#include "util/kt_log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

namespace ktplace::tcl {

namespace {

constexpr int maxDepth = 64;

std::string toLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

char unescape(char c) {
    switch (c) {
        case 'n':
            return '\n';
        case 't':
            return '\t';
        case 'r':
            return '\r';
        default:
            return c;
    }
}

bool isVariableStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_' || c == ':';
}

bool isVariableChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == ':';
}

// A `\` at the end of a line, spaces and all, continues the command onto the
// next one. Anything else after a backslash is an escape.
bool isContinuation(const std::string &s, std::size_t at) {
    if (at + 1 >= s.size() || s[at] != '\\') {
        return false;
    }
    std::size_t j = at + 1;
    while (j < s.size() && (s[j] == ' ' || s[j] == '\t' || s[j] == '\r')) {
        ++j;
    }
    return j < s.size() && s[j] == '\n';
}

// Whether a list element has to be braced or quoted to survive a round trip.
bool needsQuoting(const Value &item) {
    if (item.kind() == Value::Kind::String) {
        const std::string &text = item.asString();
        if (text.empty()) {
            return true;
        }
        return text.find_first_of(" \t\n\r{}[]\\") != std::string::npos;
    }
    return item.isList();
}

std::string quote(const std::string &text) {
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out + "\"";
}

const std::vector<Value> &emptyList() {
    static const std::vector<Value> empty;
    return empty;
}

// ---------------------------------------------------------------------------
// Splitting
// ---------------------------------------------------------------------------

// Reads one word, starting at cursor. Sets `cursor` just past the word. Returns false
// when there is no word there (end of input, or a command separator).
bool readWord(const std::string &s, std::size_t *pos, Word *word) {
    if (*pos >= s.size()) {
        return false;
    }
    const char c = s[*pos];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';') {
        return false;
    }
    word->literal = false;
    word->text.clear();
    if (c == '"') {
        ++*pos;
        while (*pos < s.size() && s[*pos] != '"') {
            if (s[*pos] == '\\' && *pos + 1 < s.size()) {
                word->text += unescape(s[*pos + 1]);
                *pos += 2;
                continue;
            }
            word->text += s[*pos];
            ++*pos;
        }
        if (*pos < s.size()) {
            ++*pos;  // the closing quote
        }
        return true;
    }
    if (c == '{') {
        word->literal = true;
        ++*pos;
        int depth = 1;
        while (*pos < s.size() && depth > 0) {
            if (s[*pos] == '\\' && *pos + 1 < s.size()) {
                // A backslash-brace is escaped, so it does not change the depth.
                word->text += s[*pos + 1];
                *pos += 2;
                continue;
            }
            if (s[*pos] == '{') {
                ++depth;
            } else if (s[*pos] == '}') {
                --depth;
                if (depth == 0) {
                    ++*pos;
                    break;
                }
            }
            word->text += s[*pos];
            ++*pos;
        }
        return true;
    }
    // A bracketed command nests: spaces inside [ ... ] belong to the command and
    // not to this word, so `[expr {a + b}]` is one word and not four.
    int brackets = 0;
    while (*pos < s.size()) {
        const char d = s[*pos];
        if (brackets == 0 && (d == ' ' || d == '\t' || d == '\n' || d == '\r' || d == ';')) {
            break;
        }
        if (d == '[') {
            ++brackets;
        } else if (d == ']' && brackets > 0) {
            --brackets;
        }
        if (d == '\\' && *pos + 1 < s.size()) {
            if (isContinuation(s, *pos)) {
                break;
            }
            word->text += unescape(s[*pos + 1]);
            *pos += 2;
            continue;
        }
        word->text += d;
        ++*pos;
    }
    return true;
}

// Splits a command into words. A `#` word with nothing before it is a comment
// and makes the whole command disappear.
std::vector<Word> splitCommand(const std::string &script, std::size_t begin, std::size_t end) {
    std::vector<Word> words;
    std::size_t pos = begin;
    while (pos < end) {
        const char c = script[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ';') {
            ++pos;
            continue;
        }
        if (c == '\\' && isContinuation(script, pos)) {
            pos += 2;
            while (pos < end &&
                   (script[pos] == ' ' || script[pos] == '\t' || script[pos] == '\r')) {
                ++pos;
            }
            if (pos < end && script[pos] == '\n') {
                ++pos;
            }
            continue;
        }
        Word word;
        if (!readWord(script, &pos, &word)) {
            break;
        }
        words.push_back(std::move(word));
    }
    // A bare '#' where a command name belongs starts a comment and the rest of
    // the command is not a command. A braced {#} is a word, not a comment.
    if (!words.empty() && !words[0].literal && words[0].text == "#") {
        return {};
    }
    return words;
}

// ---------------------------------------------------------------------------
// expr
// ---------------------------------------------------------------------------

class ExprParser {
public:
    ExprParser(const std::function<Value(const std::string &)> &lookup, const std::string &text)
        : resolve(lookup), source(text) {}

    Value parse() {
        Value out = orExpr();
        skip();
        if (cursor != source.size()) {
            throw Error("expr: unexpected '" + source.substr(cursor) + "'");
        }
        return out;
    }

private:
    void skip() {
        while (cursor < source.size() &&
               std::isspace(static_cast<unsigned char>(source[cursor])) != 0) {
            ++cursor;
        }
    }

    bool accept(const char *token) {
        skip();
        const std::size_t n = std::strlen(token);
        if (source.compare(cursor, n, token) != 0) {
            return false;
        }
        // A longer operator that starts the same way must not be truncated:
        // `==` is not a prefix-match for `=`.
        cursor += n;
        return true;
    }

    bool peek(const char *token) {
        skip();
        return source.compare(cursor, std::strlen(token), token) == 0;
    }

    Value orExpr() {
        Value left = andExpr();
        for (;;) {
            skip();
            if (peek("||")) {
                cursor += 2;
                const Value right = andExpr();
                left = Value::boolean(left.asBool() || right.asBool());
                continue;
            }
            return left;
        }
    }

    Value andExpr() {
        Value left = compareExpr();
        for (;;) {
            skip();
            if (peek("&&")) {
                cursor += 2;
                const Value right = compareExpr();
                left = Value::boolean(left.asBool() && right.asBool());
                continue;
            }
            return left;
        }
    }

    Value compareExpr() {
        Value left = sumExpr();
        for (;;) {
            skip();
            static const char *ops[] = {"==", "!=", "<=", ">=", "<", ">"};
            bool matched = false;
            for (const char *op : ops) {
                if (peek(op)) {
                    cursor += std::strlen(op);
                    const Value right = sumExpr();
                    const int cmp = compare(left, right);
                    const bool result = std::strcmp(op, "==") == 0   ? cmp == 0
                                        : std::strcmp(op, "!=") == 0 ? cmp != 0
                                        : std::strcmp(op, "<=") == 0 ? cmp <= 0
                                        : std::strcmp(op, ">=") == 0 ? cmp >= 0
                                        : std::strcmp(op, "<") == 0  ? cmp < 0
                                                                     : cmp > 0;
                    left = Value::boolean(result);
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                return left;
            }
        }
    }

    static int compare(const Value &a, const Value &b) {
        if (a.isInt() && b.isInt()) {
            const std::int64_t x = a.asInt();
            const std::int64_t y = b.asInt();
            return x < y ? -1 : (x > y ? 1 : 0);
        }
        const double x = a.asDouble();
        const double y = b.asDouble();
        return x < y ? -1 : (x > y ? 1 : 0);
    }

    Value sumExpr() {
        Value left = termExpr();
        for (;;) {
            skip();
            if (cursor >= source.size()) {
                return left;
            }
            const char op = source[cursor];
            if (op != '+' && op != '-') {
                return left;
            }
            ++cursor;
            const Value right = termExpr();
            if (op == '+') {
                left = arith(
                    left, right,
                    [](double a, double b) {
                        return a + b;
                    },
                    [](std::int64_t a, std::int64_t b) {
                        return a + b;
                    });
            } else {
                left = arith(
                    left, right,
                    [](double a, double b) {
                        return a - b;
                    },
                    [](std::int64_t a, std::int64_t b) {
                        return a - b;
                    });
            }
        }
    }

    static Value arith(const Value &a, const Value &b, std::function<double(double, double)> fd,
                       std::function<std::int64_t(std::int64_t, std::int64_t)> fi) {
        if (a.isInt() && b.isInt()) {
            return Value::integer(fi(a.asInt(), b.asInt()));
        }
        return Value::real(fd(a.asDouble(), b.asDouble()));
    }

    Value termExpr() {
        Value left = unaryExpr();
        for (;;) {
            skip();
            if (cursor >= source.size()) {
                return left;
            }
            const char op = source[cursor];
            if (op != '*' && op != '/' && op != '%') {
                return left;
            }
            ++cursor;
            const Value right = unaryExpr();
            if (op == '*') {
                left = arith(
                    left, right,
                    [](double a, double b) {
                        return a * b;
                    },
                    [](std::int64_t a, std::int64_t b) {
                        return a * b;
                    });
            } else if (op == '/') {
                const double d = right.asDouble();
                if (d == 0.0) {
                    throw Error("expr: divide by zero");
                }
                left = Value::real(left.asDouble() / d);
            } else {
                const std::int64_t d = right.asInt();
                if (d == 0) {
                    throw Error("expr: divide by zero");
                }
                left = Value::integer(left.asInt() % d);
            }
        }
    }

    Value unaryExpr() {
        skip();
        if (cursor < source.size() && source[cursor] == '!') {
            ++cursor;
            return Value::boolean(!unaryExpr().asBool());
        }
        if (cursor < source.size() && source[cursor] == '-') {
            ++cursor;
            const Value v = unaryExpr();
            return v.isInt() ? Value::integer(-v.asInt()) : Value::real(-v.asDouble());
        }
        return primaryExpr();
    }

    Value primaryExpr() {
        skip();
        if (cursor >= source.size()) {
            throw Error("expr: unexpected end of expression");
        }
        if (source[cursor] == '(') {
            ++cursor;
            const Value inner = orExpr();
            skip();
            if (cursor >= source.size() || source[cursor] != ')') {
                throw Error("expr: missing ')'");
            }
            ++cursor;
            return inner;
        }
        if (source[cursor] == '"') {
            ++cursor;
            std::string text;
            while (cursor < source.size() && source[cursor] != '"') {
                text += source[cursor++];
            }
            ++cursor;
            return parseLiteral(text);
        }
        if (std::isdigit(static_cast<unsigned char>(source[cursor])) != 0 ||
            (source[cursor] == '.' && cursor + 1 < source.size() &&
             std::isdigit(static_cast<unsigned char>(source[cursor + 1])) != 0)) {
            const std::size_t begin = cursor;
            while (cursor < source.size() &&
                   (std::isdigit(static_cast<unsigned char>(source[cursor])) != 0 ||
                    source[cursor] == '.' || source[cursor] == 'e' || source[cursor] == 'E' ||
                    ((source[cursor] == '+' || source[cursor] == '-') &&
                     (source[cursor - 1] == 'e' || source[cursor - 1] == 'E')))) {
                ++cursor;
            }
            return parseLiteral(source.substr(begin, cursor - begin));
        }
        if (isVariableStart(source[cursor])) {
            const std::size_t begin = cursor;
            while (cursor < source.size() && isVariableChar(source[cursor])) {
                ++cursor;
            }
            const std::string name = source.substr(begin, cursor - begin);
            // A function call: abs(-1), max(a, b).
            if (cursor < source.size() && source[cursor] == '(') {
                ++cursor;
                std::vector<double> args;
                for (;;) {
                    skip();
                    if (cursor < source.size() && source[cursor] == ')') {
                        ++cursor;
                        break;
                    }
                    args.push_back(orExpr().asDouble());
                    skip();
                    if (cursor < source.size() && source[cursor] == ',') {
                        ++cursor;
                    }
                }
                return applyFunction(name, args);
            }
            return resolve(name);
        }
        throw Error("expr: unexpected '" + source.substr(cursor) + "'");
    }

    Value applyFunction(const std::string &name, const std::vector<double> &args) {
        if (name == "abs") {
            return args.size() == 1 && args[0] < 0.0 ? Value::real(-args[0]) : Value::real(args[0]);
        }
        if (name == "sqrt") {
            return Value::real(std::sqrt(args.at(0)));
        }
        if (name == "floor") {
            return Value::real(std::floor(args.at(0)));
        }
        if (name == "ceil") {
            return Value::real(std::ceil(args.at(0)));
        }
        if (name == "min") {
            return Value::real(*std::min_element(args.begin(), args.end()));
        }
        if (name == "max") {
            return Value::real(*std::max_element(args.begin(), args.end()));
        }
        if (name == "pow") {
            return Value::real(std::pow(args.at(0), args.at(1)));
        }
        throw Error("expr: unknown function \"" + name + "\"");
    }

    const std::function<Value(const std::string &)> &resolve;
    const std::string &source;
    std::size_t cursor = 0;
};

// A word already split by the parser becomes a Value: a braced word is the string
// it holds, anything else is substituted and then read as a literal.
Value wordValue(Interpreter &self, const Word &word) {
    return word.literal ? Value::string(word.text) : parseLiteral(self.substitute(word.text));
}

// A list argument: already a list, or text to be split.
std::vector<Value> listValue(Interpreter &self, const Word &word) {
    if (!word.literal) {
        const Value v = parseLiteral(self.substitute(word.text));
        if (v.isList()) {
            return v.asList();
        }
        return self.splitList(v.asString());
    }
    return self.splitList(word.text);
}

std::string shellCommand(Interpreter &self, const Arguments &args, std::size_t first) {
    std::string command;
    for (std::size_t i = first; i < args.size(); ++i) {
        command += command.empty() ? "" : " ";
        command += args[i].literal ? args[i].text : self.substitute(args[i].text);
    }
    return command;
}

Value runShellCommand(const std::string &command) {
    FILE *pipe = popen((command + " 2>&1").c_str(), "r");
    if (pipe == nullptr) {
        throw Error(std::format("exec: cannot run \"{}\"", command));
    }
    std::string output;
    std::array<char, 4096> chunk{};
    while (const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), pipe)) {
        output.append(chunk.data(), got);
    }
    const int status = pclose(pipe);
    while (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }
    if (status != 0) {
        throw Error(output.empty() ? std::format("exec: \"{}\" failed", command) : output);
    }
    return Value::string(output);
}

}  // namespace

// --------------------------------------------------------------------------
// Value
// --------------------------------------------------------------------------

bool operator==(const Value &left, const Value &right) {
    return left.asString() == right.asString();
}

Value Value::integer(std::int64_t v) {
    Value out;
    out.kindOf = Kind::Int;
    out.intValue = v;
    return out;
}

Value Value::real(double v) {
    Value out;
    out.kindOf = Kind::Double;
    out.doubleValue = v;
    return out;
}

Value Value::boolean(bool v) {
    Value out;
    out.kindOf = Kind::Bool;
    out.boolValue = v;
    return out;
}

Value Value::list(std::vector<Value> v) {
    Value out;
    out.kindOf = Kind::List;
    out.listValue = std::move(v);
    return out;
}

Value Value::string(std::string v) {
    Value out;
    out.kindOf = Kind::String;
    out.stringValue = std::move(v);
    return out;
}

std::int64_t Value::asInt() const noexcept {
    switch (kindOf) {
        case Kind::Int:
            return intValue;
        case Kind::Double:
            return static_cast<std::int64_t>(doubleValue);
        case Kind::Bool:
            return boolValue ? 1 : 0;
        case Kind::String:
            return std::strtoll(stringValue.c_str(), nullptr, 10);
        case Kind::List:
            return static_cast<std::int64_t>(listValue.size());
    }
    return 0;
}

double Value::asDouble() const noexcept {
    switch (kindOf) {
        case Kind::Int:
            return static_cast<double>(intValue);
        case Kind::Double:
            return doubleValue;
        case Kind::Bool:
            return boolValue ? 1.0 : 0.0;
        case Kind::String:
            return std::strtod(stringValue.c_str(), nullptr);
        case Kind::List:
            return static_cast<double>(listValue.size());
    }
    return 0.0;
}

bool Value::asBool() const noexcept {
    switch (kindOf) {
        case Kind::Int:
            return intValue != 0;
        case Kind::Double:
            return doubleValue != 0.0;
        case Kind::Bool:
            return boolValue;
        case Kind::String: {
            // Tcl's rule: false, no, off and an empty word are false.
            const std::string low = toLower(stringValue);
            return !(low.empty() || low == "false" || low == "no" || low == "off");
        }
        case Kind::List:
            return !listValue.empty();
    }
    return true;
}

const std::vector<Value> &Value::asList() const noexcept {
    return kindOf == Kind::List ? listValue : emptyList();
}

std::string Value::asString() const {
    switch (kindOf) {
        case Kind::Int:
            return std::to_string(intValue);
        case Kind::Double: {
            std::ostringstream out;
            out.precision(17);
            out << doubleValue;
            return out.str();
        }
        case Kind::Bool:
            return boolValue ? "1" : "0";
        case Kind::String:
            return stringValue;
        case Kind::List: {
            // Tcl's canonical form, so a list survives a round trip through a string
            // option unchanged.
            std::string out = "{";
            for (std::size_t i = 0; i < listValue.size(); ++i) {
                if (i > 0) {
                    out += ' ';
                }
                // Tcl quotes only what has to be quoted, so a list of plain words
                // round-trips as {a b} rather than {"a" "b"}.
                const std::string &item = listValue[i].asString();
                out += needsQuoting(listValue[i]) ? quote(item) : item;
            }
            return out + "}";
        }
    }
    return {};
}

bool Value::looksNumeric() const noexcept {
    if (kindOf == Kind::String) {
        return isIntegerLiteral(stringValue) || isDoubleLiteral(stringValue);
    }
    return kindOf != Kind::List;
}

// --------------------------------------------------------------------------
// Literal typing
// --------------------------------------------------------------------------

bool isIntegerLiteral(const std::string &text) {
    if (text.empty()) {
        return false;
    }
    std::size_t i = (text[0] == '-' || text[0] == '+') ? 1 : 0;
    if (i >= text.size()) {
        return false;
    }
    for (; i < text.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

bool isDoubleLiteral(const std::string &text) {
    if (text.empty()) {
        return false;
    }
    char *end = nullptr;
    std::strtod(text.c_str(), &end);
    if (end == nullptr || end == text.c_str()) {
        return false;
    }
    while (*end != '\0') {
        if (std::isspace(static_cast<unsigned char>(*end)) == 0) {
            return false;
        }
        ++end;
    }
    return true;
}

Value parseLiteral(const std::string &text) {
    if (isIntegerLiteral(text)) {
        return Value::integer(std::strtoll(text.c_str(), nullptr, 10));
    }
    if (isDoubleLiteral(text)) {
        return Value::real(std::strtod(text.c_str(), nullptr));
    }
    if (text == "true") {
        return Value::boolean(true);
    }
    if (text == "false") {
        return Value::boolean(false);
    }
    return Value::string(text);
}

// --------------------------------------------------------------------------
// Interpreter
// --------------------------------------------------------------------------

Interpreter::Interpreter() {
    installBuiltins();
}

void Interpreter::installBuiltins() {
    registerCommand(
        {.name = "set", .usage = "set <var> [value]", .summary = "read a variable, or set it"},
        [](Interpreter &self, const Arguments &args, Value &result) -> int {
            if (args.size() == 2) {
                result = self.variable(args[1].text);
                return 0;
            }
            if (args.size() != 3) {
                throw Error("set: expected 2 or 3 arguments");
            }
            result = wordValue(self, args[2]);
            self.setVariable(args[1].text, result);
            return 0;
        });

    registerCommand({.name = "unset", .usage = "unset <var> ...", .summary = "remove variables"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        for (std::size_t i = 1; i < args.size(); ++i) {
                            self.unsetVariable(args[i].text);
                        }
                        return 0;
                    });

    registerCommand(
        {.name = "puts", .usage = "puts [stderr] <text>", .summary = "print to the log"},
        [](Interpreter &self, const Arguments &args, Value &) -> int {
            std::string text;
            if (args.size() == 2) {
                text = self.substitute(args[1].text, 0);
            } else if (args.size() == 3) {
                const Value channel = parseLiteral(self.substitute(args[1].text, 0));
                text = channel.asString() == "stderr"
                           ? "stderr: " + self.substitute(args[2].text, 0)
                           : self.substitute(args[2].text, 0);
            } else {
                throw Error("puts: expected 1 or 2 arguments");
            }
            ktlog.echo("{}", text);
            return 0;
        });

    registerCommand({.name = "expr",
                     .usage = "expr <expression>",
                     .summary = "evaluate an arithmetic or logical expression"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() < 2) {
                            throw Error("expr: expected an expression");
                        }
                        std::string text;
                        for (std::size_t i = 1; i < args.size(); ++i) {
                            if (i > 1) {
                                text += ' ';
                            }
                            text += self.substitute(args[i].text, 0);
                        }
                        ExprParser parser(
                            [&self](const std::string &name) {
                                return self.variable(name);
                            },
                            text);
                        result = parser.parse();
                        return 0;
                    });

    registerCommand(
        {.name = "incr", .usage = "incr <var> [amount]", .summary = "add to an integer variable"},
        [](Interpreter &self, const Arguments &args, Value &) -> int {
            if (args.size() < 2 || args.size() > 3) {
                throw Error("incr: expected 1 or 2 arguments");
            }
            const std::int64_t step =
                args.size() == 3 ? parseLiteral(self.substitute(args[2].text, 0)).asInt() : 1;
            const std::int64_t current =
                self.hasVariable(args[1].text) ? self.variable(args[1].text).asInt() : 0;
            self.setVariable(args[1].text, Value::integer(current + step));
            return 0;
        });

    registerCommand({.name = "if",
                     .usage = "if <cond> <body> [elseif <cond> <body>] [else <body>]",
                     .summary = "run a body when a condition holds"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        // if cond ?then? body elseif cond body ... ?else? body
                        std::size_t i = 1;
                        for (;;) {
                            if (i >= args.size()) {
                                throw Error("if: no condition");
                            }
                            const std::string cond = self.substitute(args[i].text, 0);
                            i++;
                            if (i < args.size() && args[i].literal && args[i].text == "then") {
                                i++;
                            }
                            if (i >= args.size()) {
                                throw Error("if: missing body");
                            }
                            ExprParser parser(
                                [&self](const std::string &n) {
                                    return self.variable(n);
                                },
                                cond);
                            if (parser.parse().asBool()) {
                                (void)self.eval(args[i].text, 0);
                                return 0;
                            }
                            i++;
                            if (i >= args.size()) {
                                return 0;
                            }
                            if (args[i].literal && args[i].text == "elseif") {
                                i++;
                                continue;
                            }
                            if (args[i].literal && args[i].text == "else") {
                                i++;
                                if (i >= args.size()) {
                                    throw Error("if: missing else body");
                                }
                                (void)self.eval(args[i].text, 0);
                                return 0;
                            }
                            throw Error("if: expected 'elseif' or 'else'");
                        }
                    });

    registerCommand({.name = "while",
                     .usage = "while <cond> <body>",
                     .summary = "repeat a body while a condition holds"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        if (args.size() != 3) {
                            throw Error("while: expected a condition and a body");
                        }
                        // The condition is re-substituted every pass: substituting it once would
                        // freeze the loop variable at its starting value and spin forever.
                        for (int guard = 0; guard < 1000000; ++guard) {
                            const std::string cond = self.substitute(args[1].text, 0);
                            ExprParser parser(
                                [&self](const std::string &n) {
                                    return self.variable(n);
                                },
                                cond);
                            if (!parser.parse().asBool()) {
                                return 0;
                            }
                            (void)self.eval(args[2].text, 0);
                        }
                        throw Error("while: did not terminate");
                    });

    registerCommand({.name = "foreach",
                     .usage = "foreach <var> <list> <body>",
                     .summary = "run a body for each list element"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        if (args.size() != 4) {
                            throw Error("foreach: expected var, list and body");
                        }
                        const std::vector<std::string> names = self.parseListNames(args[1].text);
                        const std::vector<Value> items = listValue(self, args[2]);
                        if (names.size() == 1) {
                            for (const Value &item : items) {
                                self.setVariable(names[0], item);
                                (void)self.eval(args[3].text, 0);
                            }
                            return 0;
                        }
                        for (std::size_t i = 0; i + names.size() <= items.size();
                             i += names.size()) {
                            for (std::size_t n = 0; n < names.size(); ++n) {
                                self.setVariable(names[n], items[i + n]);
                            }
                            (void)self.eval(args[3].text, 0);
                        }
                        return 0;
                    });

    registerCommand({.name = "list", .usage = "list <value> ...", .summary = "build a list"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        std::vector<Value> items;
                        for (std::size_t i = 1; i < args.size(); ++i) {
                            items.push_back(wordValue(self, args[i]));
                        }
                        result = Value::list(std::move(items));
                        return 0;
                    });

    registerCommand(
        {.name = "lindex", .usage = "lindex <list> <index>", .summary = "one element of a list"},
        [](Interpreter &self, const Arguments &args, Value &result) -> int {
            if (args.size() != 3) {
                throw Error("lindex: expected a list and an index");
            }
            const std::vector<Value> items = listValue(self, args[1]);
            const std::int64_t index = wordValue(self, args[2]).asInt();
            if (index < 0 || static_cast<std::size_t>(index) >= items.size()) {
                result = Value::string("");
                return 0;
            }
            result = items[static_cast<std::size_t>(index)];
            return 0;
        });

    registerCommand({.name = "llength",
                     .usage = "llength <list>",
                     .summary = "the number of elements in a list"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() != 2) {
                            throw Error("llength: expected a list");
                        }
                        const std::vector<Value> items = listValue(self, args[1]);
                        result = Value::integer(static_cast<std::int64_t>(items.size()));
                        return 0;
                    });

    registerCommand({.name = "lappend",
                     .usage = "lappend <var> <value> ...",
                     .summary = "append to a list variable"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() < 2) {
                            throw Error("lappend: expected a variable");
                        }
                        std::vector<Value> items;
                        if (self.hasVariable(args[1].text)) {
                            items = self.variable(args[1].text).asList();
                        }
                        for (std::size_t i = 2; i < args.size(); ++i) {
                            items.push_back(wordValue(self, args[i]));
                        }
                        const Value out = Value::list(std::move(items));
                        self.setVariable(args[1].text, out);
                        result = out;
                        return 0;
                    });

    registerCommand(
        {.name = "string",
         .usage = "string <subcommand> ...",
         .summary = "string length, equal, match, toupper, ..."},
        [](Interpreter &self, const Arguments &args, Value &result) -> int {
            if (args.size() < 3) {
                throw Error("string: expected a subcommand");
            }
            const std::string sub =
                args[1].literal ? args[1].text : self.substitute(args[1].text, 0);
            const Value subject = parseLiteral(self.substitute(args[2].text, 0));
            const std::string text = subject.asString();
            if (sub == "length") {
                result = Value::integer(static_cast<std::int64_t>(text.size()));
                return 0;
            }
            if (sub == "tolower") {
                result = Value::string(toLower(text));
                return 0;
            }
            if (sub == "toupper") {
                result = Value::string([&] {
                    std::string out = text;
                    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                        return static_cast<char>(std::toupper(c));
                    });
                    return out;
                }());
                return 0;
            }
            if (sub == "trim") {
                const std::size_t b = text.find_first_not_of(" \t\n");
                const std::size_t e = text.find_last_not_of(" \t\n");
                result = Value::string(b == std::string::npos ? "" : text.substr(b, e - b + 1));
                return 0;
            }
            if (sub == "equal") {
                if (args.size() < 4) {
                    throw Error("string equal: expected two operands");
                }
                result = Value::boolean(text == self.substitute(args[3].text, 0));
                return 0;
            }
            if (sub == "match") {
                if (args.size() < 4) {
                    throw Error("string match: expected a pattern and a string");
                }
                const std::string pattern = self.substitute(args[3].text, 0);
                result = Value::boolean(globMatch(pattern, text));
                return 0;
            }
            if (sub == "first" || sub == "last") {
                if (args.size() < 4) {
                    throw Error("string " + sub + ": expected a needle");
                }
                const std::string needle = self.substitute(args[3].text, 0);
                if (sub == "first") {
                    const std::size_t at = text.find(needle);
                    result = Value::integer(
                        at == std::string::npos ? -1 : static_cast<std::int64_t>(at));
                } else {
                    const std::size_t at = text.rfind(needle);
                    result = Value::integer(
                        at == std::string::npos ? -1 : static_cast<std::int64_t>(at));
                }
                return 0;
            }
            throw Error("string: unknown subcommand \"" + sub + "\"");
        });

    registerCommand({.name = "info",
                     .usage = "info exists <var> | info commands",
                     .summary = "ask about variables and commands"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() < 2) {
                            throw Error("info: expected a subcommand");
                        }
                        const std::string sub = args[1].text;
                        if (sub == "exists") {
                            if (args.size() != 3) {
                                throw Error("info exists: expected a variable name");
                            }
                            result = Value::boolean(self.hasVariable(args[2].text));
                            return 0;
                        }
                        if (sub == "commands") {
                            result = Value::list([&self] {
                                std::vector<Value> names;
                                for (const auto &entry : self.commands()) {
                                    names.push_back(Value::string(entry.first));
                                }
                                return names;
                            }());
                            return 0;
                        }
                        throw Error("info: unknown subcommand \"" + sub + "\"");
                    });

    registerCommand({.name = "error", .usage = "error <message>", .summary = "raise an error"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        if (args.size() < 2) {
                            throw Error("error: expected a message");
                        }
                        throw Error(self.substitute(args[1].text, 0));
                    });

    registerCommand({.name = "help",
                     .usage = "help [command]",
                     .summary = "list the commands, or show one command's usage"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() > 2) {
                            throw Error("wrong # args: should be \"help [command]\"");
                        }
                        result = Value::string(
                            self.helpText(args.size() == 2 ? self.substitute(args[1].text) : ""));
                        return 0;
                    });

    registerCommand({.name = "exec",
                     .usage = "exec <command> [arg ...]",
                     .summary = "run a shell command and return its output"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() < 2) {
                            throw Error("wrong # args: should be \"exec <command> [arg ...]\"");
                        }
                        result = runShellCommand(shellCommand(self, args, 1));
                        return 0;
                    });

    registerCommand({.name = "ls",
                     .usage = "ls [option ...] [path ...]",
                     .summary = "list files, as the system ls does"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        result = runShellCommand("ls " + shellCommand(self, args, 1));
                        return 0;
                    });

    registerCommand({.name = "pwd", .usage = "pwd", .summary = "the current directory"},
                    [](Interpreter &, const Arguments &args, Value &result) -> int {
                        if (args.size() != 1) {
                            throw Error("wrong # args: should be \"pwd\"");
                        }
                        result = Value::string(std::filesystem::current_path().string());
                        return 0;
                    });

    registerCommand({.name = "cd",
                     .usage = "cd [dir]",
                     .summary = "change the current directory (default: home)"},
                    [](Interpreter &self, const Arguments &args, Value &result) -> int {
                        if (args.size() > 2) {
                            throw Error("wrong # args: should be \"cd [dir]\"");
                        }
                        const char *home = std::getenv("HOME");
                        const std::string dir = args.size() == 2
                                                    ? self.substitute(args[1].text)
                                                    : std::string(home != nullptr ? home : "");
                        std::error_code ec;
                        std::filesystem::current_path(dir, ec);
                        if (ec) {
                            throw Error(std::format("cd: {}: {}", dir, ec.message()));
                        }
                        result = Value::string(std::filesystem::current_path().string());
                        return 0;
                    });

    registerCommand({.name = "source", .usage = "source <file>", .summary = "run a Tcl script"},
                    [](Interpreter &self, const Arguments &args, Value &) -> int {
                        if (args.size() != 2) {
                            throw Error("source: expected a file name");
                        }
                        (void)self.evalFile(self.substitute(args[1].text, 0));
                        return 0;
                    });
}

void Interpreter::registerCommand(CommandHelp help, Command command) {
    commandTable[help.name] = std::move(command);
    helpTable[help.name] = std::move(help);
}

std::string Interpreter::helpText(const std::string &name) const {
    if (!name.empty()) {
        const auto found = helpTable.find(name);
        if (found == helpTable.end()) {
            throw Error(std::format("help: no command \"{}\"; try help", name));
        }
        return std::format("{}\n  {}", found->second.usage, found->second.summary);
    }
    std::size_t width = 0;
    for (const auto &[command, help] : helpTable) {
        width = std::max(width, help.usage.size());
    }
    std::string text = "Commands:";
    for (const auto &[command, help] : helpTable) {
        text += std::format("\n  {:<{}}  {}", help.usage, width, help.summary);
    }
    return text + "\nhelp <command> shows one command.";
}

bool Interpreter::hasCommand(const std::string &name) const {
    return commandTable.count(name) != 0;
}

const std::map<std::string, Command> &Interpreter::commands() const noexcept {
    return commandTable;
}

void Interpreter::setVariable(const std::string &name, Value value) {
    variableTable.assign(name, std::move(value));
}

void Interpreter::unsetVariable(const std::string &name) {
    variableTable.remove(name);
}

Value Interpreter::variable(const std::string &name) const {
    const auto found = variableTable.find(name);
    if (!found) {
        throw Error("can't read \"" + name + "\": no such variable");
    }
    return *found;
}

bool Interpreter::hasVariable(const std::string &name) const noexcept {
    return variableTable.contains(name);
}

std::string Interpreter::scriptPath() const {
    return scriptStack.empty() ? std::string() : scriptStack.back();
}

Variables Interpreter::variables() const {
    return Variables(variableTable.all());
}

// Replaces $name, ${name}, [command] and backslash escapes. Braced words never
// reach here, which is what makes them literal.
std::string Interpreter::substitute(const std::string &text, int depth) {
    std::string out;
    for (std::size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            if (isContinuation(text, i)) {
                i += 2;
                while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) {
                    i++;
                }
                if (i < text.size() && text[i] == '\n') {
                    i++;
                }
                continue;
            }
            out += unescape(text[i + 1]);
            i += 2;
            continue;
        }
        if (c == '$' && i + 1 < text.size() && isVariableStart(text[i + 1])) {
            std::size_t j = i + 1;
            std::string name;
            if (text[j] == '{') {
                const std::size_t close = text.find('}', j);
                if (close != std::string::npos) {
                    name = text.substr(j + 1, close - j - 1);
                    j = close + 1;
                }
            } else {
                const std::size_t begin = j;
                while (j < text.size() && isVariableChar(text[j])) {
                    j++;
                }
                name = text.substr(begin, j - begin);
            }
            if (name.empty()) {
                out += c;
                i++;
                continue;
            }
            out += variable(name).asString();
            i = j;
            continue;
        }
        if (c == '[') {
            const std::size_t close = matchBracket(text, i);
            const std::string inner = text.substr(i + 1, close - i - 1);
            out += eval(inner, depth + 1).asString();
            i = close + 1;
            continue;
        }
        out += c;
        i++;
    }
    return out;
}

// The ']' that closes the '[' at `open`, skipping nested brackets and quoted
// text so that `[list {a]b}]` finds the right one.
std::size_t Interpreter::matchBracket(const std::string &s, std::size_t open) const {
    int depth = 0;
    bool inQuotes = false;
    for (std::size_t i = open; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            i++;
            continue;
        }
        if (c == '"' && (i == open + 1 || s[i - 1] != '\\')) {
            inQuotes = !inQuotes;
            continue;
        }
        if (inQuotes) {
            continue;
        }
        if (c == '[') {
            depth++;
        } else if (c == ']') {
            if (--depth == 0) {
                return i;
            }
        }
    }
    throw Error("unmatched '['");
}

// Splits a Tcl list literal into its elements, honouring braces and quotes.
std::vector<Value> Interpreter::splitList(const std::string &text) const {
    std::vector<Value> out;
    std::string body = text;
    if (body.size() >= 2 && body.front() == '{' && body.back() == '}') {
        body = body.substr(1, body.size() - 2);
    }
    std::size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])) != 0) {
            i++;
        }
        if (i >= body.size()) {
            break;
        }
        std::string item;
        if (body[i] == '{') {
            int depth = 1;
            i++;
            while (i < body.size() && depth > 0) {
                if (body[i] == '{') {
                    depth++;
                } else if (body[i] == '}') {
                    depth--;
                    if (depth == 0) {
                        i++;
                        break;
                    }
                }
                item += body[i++];
            }
            out.push_back(Value::string(item));
            continue;
        }
        while (i < body.size() && std::isspace(static_cast<unsigned char>(body[i])) == 0) {
            if (body[i] == '\\' && i + 1 < body.size()) {
                item += body[i + 1];
                i += 2;
                continue;
            }
            item += body[i++];
        }
        out.push_back(parseLiteral(item));
    }
    return out;
}

std::vector<std::string> Interpreter::parseListNames(const std::string &text) const {
    std::vector<std::string> names;
    for (const Value &v : splitList(text)) {
        names.push_back(v.asString());
    }
    return names;
}

bool globMatch(const std::string &pattern, const std::string &text) {
    // Tcl's match is glob-style: * and ? are wildcards, everything else literal.
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string::npos;
    std::size_t mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            p++;
            t++;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        p++;
    }
    return p == pattern.size();
}

Value Interpreter::evalFile(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        throw Error("cannot read script \"" + path + "\"");
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    scriptStack.push_back(path);
    Value result;
    try {
        result = eval(buffer.str(), 0);
    } catch (...) {
        scriptStack.pop_back();
        throw;
    }
    scriptStack.pop_back();
    return result;
}

Value Interpreter::eval(const std::string &script, int depth) {
    if (depth > maxDepth) {
        throw Error("too many nested commands (is a command calling itself?)");
    }
    Value result;
    std::size_t pos = 0;
    while (pos < script.size()) {
        // Skip separators and comments between commands.
        while (pos < script.size()) {
            const char c = script[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == ';' || c == '\n') {
                pos++;
                continue;
            }
            if (c == '\\' && isContinuation(script, pos)) {
                pos += 2;
                while (pos < script.size() &&
                       (script[pos] == ' ' || script[pos] == '\t' || script[pos] == '\r')) {
                    pos++;
                }
                if (pos < script.size() && script[pos] == '\n') {
                    pos++;
                }
                continue;
            }
            break;
        }
        if (pos >= script.size()) {
            break;
        }
        // One command runs to the next newline or ';' outside quotes and braces.
        const std::size_t begin = pos;
        int braces = 0;
        bool inQuotes = false;
        while (pos < script.size()) {
            const char c = script[pos];
            if (c == '\\' && pos + 1 < script.size()) {
                pos += 2;
                continue;
            }
            if (c == '"' && braces == 0) {
                inQuotes = !inQuotes;
                pos++;
                continue;
            }
            if (!inQuotes) {
                if (c == '{') {
                    braces++;
                } else if (c == '}') {
                    braces--;
                } else if ((c == '\n' || c == ';') && braces == 0) {
                    break;
                }
            }
            pos++;
        }
        std::vector<Word> words = splitCommand(script, begin, pos);
        if (!words.empty()) {
            result = run(words, depth);
        }
    }
    return result;
}

Value Interpreter::invoke(const Arguments &args) {
    return run(args, 0);
}

Value Interpreter::run(const Arguments &words, int depth) {
    if (words.empty()) {
        return {};
    }
    const std::string name = words[0].literal ? words[0].text : substitute(words[0].text, depth);
    const auto it = commandTable.find(name);
    if (it == commandTable.end()) {
        throw Error("invalid command name \"" + name + "\"");
    }
    // The command name and each argument in turn, so `[a [b c]]` composes.
    Arguments args;
    args.reserve(words.size());
    for (std::size_t i = 0; i < words.size(); ++i) {
        Word word = words[i];
        if (i > 0 && !word.literal) {
            word.text = substitute(word.text, depth);
        }
        args.push_back(std::move(word));
    }
    currentCommand = name;
    Value result;
    const int rc = it->second(*this, args, result);
    if (rc != 0) {
        throw Error(name + " failed with code " + std::to_string(rc));
    }
    return result;
}

}  // namespace ktplace::tcl
