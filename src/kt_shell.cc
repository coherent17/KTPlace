#include "kt_shell.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "tcl/kt_tcl.h"
#include "util/kt_history.h"
#include "util/kt_log.h"

#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <termios.h>
#include <unistd.h>
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

std::optional<char> readByte() {
    char c = 0;
    if (::read(STDIN_FILENO, &c, 1) != 1) {
        return std::nullopt;
    }
    return c;
}

void redraw(const std::string &prompt, const std::string &line, std::size_t cursor) {
    std::cerr << '\r' << prompt << line << "\x1b[K";
    if (cursor < line.size()) {
        std::cerr << "\x1b[" << line.size() - cursor << 'D';
    }
    std::cerr << std::flush;
}

std::optional<std::string> editLine(const std::string &prompt,
                                    const std::vector<std::string> &history) {
    const RawTerminal raw;
    std::string line;
    std::string draft;
    std::size_t cursor = 0;
    std::size_t recall = history.size();
    const auto recallEntry = [&](std::size_t index) {
        if (recall == history.size()) {
            draft = line;
        }
        recall = index;
        line = recall < history.size() ? history[recall] : draft;
        cursor = line.size();
    };
    redraw(prompt, line, cursor);
    while (const std::optional<char> key = readByte()) {
        const char c = *key;
        if (c == '\r' || c == '\n') {
            std::cerr << "\r\n" << std::flush;
            return line;
        }
        if (c == 4 && line.empty()) {
            std::cerr << "\r\n" << std::flush;
            return std::nullopt;
        }
        if (c == 3) {
            std::cerr << "^C\r\n" << std::flush;
            return std::string();
        }
        if ((c == 127 || c == 8) && cursor > 0) {
            line.erase(--cursor, 1);
        } else if (c == 4 && cursor < line.size()) {
            line.erase(cursor, 1);
        } else if (c == 1) {
            cursor = 0;
        } else if (c == 5) {
            cursor = line.size();
        } else if (c == 21) {
            line.erase(0, cursor);
            cursor = 0;
        } else if (c == 11) {
            line.erase(cursor);
        } else if (c == 27) {
            const std::optional<char> kind = readByte();
            const std::optional<char> code = readByte();
            if (!kind || !code) {
                break;
            }
            if (*code == 'A' && recall > 0) {
                recallEntry(recall - 1);
            } else if (*code == 'B' && recall < history.size()) {
                recallEntry(recall + 1);
            } else if (*code == 'C' && cursor < line.size()) {
                ++cursor;
            } else if (*code == 'D' && cursor > 0) {
                --cursor;
            } else if (*code == 'H') {
                cursor = 0;
            } else if (*code == 'F') {
                cursor = line.size();
            } else if (*code >= '0' && *code <= '9') {
                const std::optional<char> tilde = readByte();
                if (tilde && *tilde == '~') {
                    if ((*code == '3') && cursor < line.size()) {
                        line.erase(cursor, 1);
                    } else if (*code == '1' || *code == '7') {
                        cursor = 0;
                    } else if (*code == '4' || *code == '8') {
                        cursor = line.size();
                    }
                }
            }
        } else if (static_cast<unsigned char>(c) >= 32) {
            line.insert(cursor++, 1, c);
        }
        redraw(prompt, line, cursor);
    }
    return std::nullopt;
}

std::optional<std::string> readLine(std::istream &in, bool interactive, const std::string &prompt,
                                    const std::vector<std::string> &history) {
    if (interactive) {
        return editLine(prompt, history);
    }
    std::cerr << prompt << std::flush;
    std::string line;
    if (!std::getline(in, line)) {
        return std::nullopt;
    }
    return line;
}

bool complete(const std::string &script) {
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

void evaluate(FlowMgr &flow, const std::string &script) {
    try {
        const std::string result = flow.evaluate(script);
        if (!result.empty()) {
            ktlog.echo("{}", result);
        }
    } catch (const tcl::Error &error) {
        ktlog.warning("{}", error.what());
    } catch (const FatalError &) {
    } catch (const std::exception &error) {
        ktlog.warning("{}", error.what());
    }
}

}  // namespace

int runShell(const kt_option &options, std::istream &in) {
    FlowMgr flow;
    flow.begin(options);
    ktlog.throwOnFatal(true);
    if (!options.inputPath.empty()) {
        evaluate(flow, "read_design {" + options.inputPath + "}");
    }
    if (!options.tclFile.empty()) {
        evaluate(flow, "source {" + options.tclFile + "}");
    }

    const bool interactive =
        &in == &std::cin && isatty(STDIN_FILENO) != 0 && isatty(STDERR_FILENO) != 0;
    CommandHistory history;
    ktlog.echo("Tcl shell: help lists the commands, exit leaves; history in {}",
               history.file().empty() ? std::string("memory only") : history.file());
    std::string buffer;
    while (true) {
        const std::optional<std::string> line = readLine(
            in, interactive, buffer.empty() ? "ktplace> " : "    ...> ", history.entries());
        if (!line) {
            break;
        }
        buffer += *line + '\n';
        if (!complete(buffer)) {
            continue;
        }
        const std::string command = trimmed(buffer);
        buffer.clear();
        history.add(command);
        if (command == "exit" || command == "quit") {
            break;
        }
        if (!command.empty()) {
            evaluate(flow, command);
        }
    }
    std::cerr << '\n';
    ktlog.throwOnFatal(false);
    flow.finish();
    return 0;
}

}  // namespace ktplace
