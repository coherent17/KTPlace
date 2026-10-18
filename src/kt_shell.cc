#include "kt_shell.h"

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "tcl/kt_tcl.h"
#include "util/kt_log.h"

#include <exception>
#include <iostream>
#include <string>

namespace ktplace {

namespace {

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

    std::string buffer;
    std::string line;
    while (true) {
        std::cerr << (buffer.empty() ? "ktplace> " : "    ...> ") << std::flush;
        if (!std::getline(in, line)) {
            break;
        }
        buffer += line + '\n';
        if (!complete(buffer)) {
            continue;
        }
        const std::string command = trimmed(buffer);
        buffer.clear();
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
