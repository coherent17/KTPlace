#include "kt_shell.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "tcl/kt_tcl.h"
#include "util/kt_history.h"
#include "util/kt_log.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

class RawTerminal {
public:
    RawTerminal() {
        tcgetattr(STDIN_FILENO, &saved);
        termios raw = saved;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    }
    ~RawTerminal() {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved);
    }
    RawTerminal(const RawTerminal &) = delete;
    RawTerminal &operator=(const RawTerminal &) = delete;

private:
    termios saved{};
};

namespace key {
constexpr char ctrlA = 1;
constexpr char ctrlC = 3;
constexpr char ctrlD = 4;
constexpr char ctrlE = 5;
constexpr char backspace = 8;
constexpr char ctrlK = 11;
constexpr char ctrlU = 21;
constexpr char escape = 27;
constexpr char del = 127;
}  // namespace key

std::optional<char> readByte() {
    char c = 0;
    if (::read(STDIN_FILENO, &c, 1) != 1) {
        return std::nullopt;
    }
    return c;
}

class LineEditor {
public:
    LineEditor(std::string promptText, const std::vector<std::string> &entries)
        : prompt(std::move(promptText)), history(entries), recall(entries.size()) {}

    std::optional<std::string> read() {
        const RawTerminal raw;
        redraw();
        while (const std::optional<char> c = readByte()) {
            if (*c == '\r' || *c == '\n') {
                return endLine("", line);
            }
            if (*c == key::ctrlD && line.empty()) {
                return endLine("", std::nullopt);
            }
            if (*c == key::ctrlC) {
                return endLine("^C", std::string());
            }
            if (*c == key::escape && !handleEscape()) {
                break;
            }
            handleKey(*c);
            redraw();
        }
        return std::nullopt;
    }

private:
    std::string prompt;
    const std::vector<std::string> &history;
    std::string line;
    std::string draft;
    std::size_t cursor = 0;
    std::size_t recall;

    static std::optional<std::string> endLine(std::string_view echo,
                                              std::optional<std::string> result) {
        std::cerr << echo << "\r\n" << std::flush;
        return result;
    }

    void redraw() const {
        std::cerr << '\r' << prompt << line << "\x1b[K";
        if (cursor < line.size()) {
            std::cerr << "\x1b[" << line.size() - cursor << 'D';
        }
        std::cerr << std::flush;
    }

    void handleKey(char c) {
        if ((c == key::del || c == key::backspace) && cursor > 0) {
            line.erase(--cursor, 1);
        } else if (c == key::ctrlD) {
            deleteUnderCursor();
        } else if (c == key::ctrlA) {
            cursor = 0;
        } else if (c == key::ctrlE) {
            cursor = line.size();
        } else if (c == key::ctrlU) {
            line.erase(0, cursor);
            cursor = 0;
        } else if (c == key::ctrlK) {
            line.erase(cursor);
        } else if (static_cast<unsigned char>(c) >= ' ' && c != key::del) {
            line.insert(cursor++, 1, c);
        }
    }

    bool handleEscape() {
        const std::optional<char> kind = readByte();
        const std::optional<char> code = readByte();
        if (!kind || !code) {
            return false;
        }
        switch (*code) {
            case 'A':
                recallPrevious();
                break;
            case 'B':
                recallNext();
                break;
            case 'C':
                cursor = std::min(cursor + 1, line.size());
                break;
            case 'D':
                cursor = cursor > 0 ? cursor - 1 : 0;
                break;
            case 'H':
                cursor = 0;
                break;
            case 'F':
                cursor = line.size();
                break;
            default:
                if (*code >= '0' && *code <= '9') {
                    handleTildeSequence(*code);
                }
        }
        return true;
    }

    void handleTildeSequence(char code) {
        const std::optional<char> tilde = readByte();
        if (!tilde || *tilde != '~') {
            return;
        }
        if (code == '3') {
            deleteUnderCursor();
        } else if (code == '1' || code == '7') {
            cursor = 0;
        } else if (code == '4' || code == '8') {
            cursor = line.size();
        }
    }

    void deleteUnderCursor() {
        if (cursor < line.size()) {
            line.erase(cursor, 1);
        }
    }

    void recallPrevious() {
        if (recall > 0) {
            recallEntry(recall - 1);
        }
    }

    void recallNext() {
        if (recall < history.size()) {
            recallEntry(recall + 1);
        }
    }

    void recallEntry(std::size_t index) {
        if (recall == history.size()) {
            draft = line;
        }
        recall = index;
        line = recall < history.size() ? history[recall] : draft;
        cursor = line.size();
    }
};

std::optional<std::string> readLine(std::istream &in, bool interactive, const std::string &prompt,
                                    const std::vector<std::string> &history) {
    if (interactive) {
        return LineEditor(prompt, history).read();
    }
    std::cerr << prompt << std::flush;
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    return line;
}

bool isComplete(const std::string &script) {
    int braces = 0;
    int brackets = 0;
    bool quoted = false;
    for (std::size_t i = 0; i < script.size(); ++i) {
        const char c = script[i];
        if (c == '\\') {
            ++i;
        } else if (c == '"' && braces == 0) {
            quoted = !quoted;
        } else if (c == '{') {
            ++braces;
        } else if (c == '}') {
            --braces;
        } else if (c == '[' && braces == 0) {
            ++brackets;
        } else if (c == ']' && braces == 0) {
            --brackets;
        }
    }
    return braces <= 0 && brackets <= 0 && !quoted;
}

std::string trimmed(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

bool evaluateAndReport(FlowMgr &flow, const std::string &script) {
    try {
        const std::string result = flow.evaluate(script);
        if (!result.empty()) {
            ktlog.echo("{}", result);
        }
        return true;
    } catch (const tcl::Error &error) {
        ktlog.warning("{}", error.what());
    } catch (const FatalError &) {
    } catch (const std::exception &error) {
        ktlog.warning("{}", error.what());
    }
    return false;
}

std::optional<int> exitStatus(std::string_view command) {
    if (command == "exit" || command == "quit") {
        return 0;
    }
    if (!command.starts_with("exit ")) {
        return std::nullopt;
    }
    const std::string_view code = command.substr(command.find_first_not_of(' ', 4));
    int status = 0;
    const auto [end, error] = std::from_chars(code.data(), code.data() + code.size(), status);
    if (error != std::errc{} || end != code.data() + code.size()) {
        return std::nullopt;
    }
    return status;
}

}  // namespace

int runShell(const kt_option &options, std::istream &in) {
    FlowMgr flow;
    flow.begin(options);
    ktlog.throwOnFatal(true);
    bool startedCleanly = true;
    if (!options.inputPath.empty()) {
        startedCleanly = evaluateAndReport(flow, "read_design {" + options.inputPath + "}");
    }
    if (!options.tclFile.empty()) {
        startedCleanly =
            evaluateAndReport(flow, "source {" + options.tclFile + "}") && startedCleanly;
    }

    const bool interactive =
        &in == &std::cin && isatty(STDIN_FILENO) != 0 && isatty(STDERR_FILENO) != 0;
    CommandHistory history;
    ktlog.echo("Tcl shell: help lists the commands, exit leaves; history in {}",
               history.file().empty() ? std::string("memory only") : history.file());
    std::string buffer;
    std::optional<int> status;
    while (!status) {
        const std::optional<std::string> line = readLine(
            in, interactive, buffer.empty() ? "ktplace> " : "    ...> ", history.entries());
        if (!line) {
            break;
        }
        buffer += *line + '\n';
        if (!isComplete(buffer)) {
            continue;
        }
        const std::string command = trimmed(buffer);
        buffer.clear();
        history.add(command);
        status = exitStatus(command);
        if (!status && !command.empty()) {
            evaluateAndReport(flow, command);
        }
    }
    std::cerr << '\n';
    ktlog.throwOnFatal(false);
    flow.finish();
    return status.value_or(startedCleanly ? 0 : 1);
}

}  // namespace ktplace
