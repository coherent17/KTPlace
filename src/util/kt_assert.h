#pragma once

#include <stdexcept>
#include <string>

namespace ktplace {

class AssertionFailed : public std::runtime_error {
public:
    AssertionFailed(const char *file, int line, const char *func, const char *expr)
        : std::runtime_error(std::string(file != nullptr ? file : "?") + ":" +
                             std::to_string(line) + ": " + (func != nullptr ? func : "?") +
                             ": assertion failed: " + (expr != nullptr ? expr : "?")),
          sourceFile(file),
          sourceLine(line),
          functionName(func),
          conditionText(expr) {}

    [[nodiscard]] const char *file() const {
        return sourceFile;
    }
    [[nodiscard]] int line() const {
        return sourceLine;
    }
    [[nodiscard]] const char *function() const {
        return functionName;
    }
    [[nodiscard]] const char *expression() const {
        return conditionText;
    }

private:
    const char *sourceFile;
    int sourceLine;
    const char *functionName;
    const char *conditionText;
};

void reportAssertion(const AssertionFailed &e);

}  // namespace ktplace

#define KTASSERT(cond)                                                             \
    do {                                                                           \
        if (!(cond)) {                                                             \
            throw ::ktplace::AssertionFailed(__FILE__, __LINE__, __func__, #cond); \
        }                                                                          \
    } while (false)
