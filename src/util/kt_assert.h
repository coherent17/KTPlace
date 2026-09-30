/**
 * @file kt_assert.h
 * @brief Assertions that report where they failed and unwind.
 *
 * Separate from the logging header on purpose: an assertion is a control-flow
 * tool, not a diagnostic, and a caller that wants to be sure a precondition held
 * should not have to pull in the logging machinery to say so.
 */

#pragma once

#include <stdexcept>
#include <string>

namespace ktplace {

/**
 * @brief Thrown by KTASSERT, carrying where it failed and what it checked.
 *
 * The pieces are kept alongside the message because a caller handling the failure
 * has no way to recover them from the text, and re-deriving them means repeating
 * the parse.
 */
class AssertionFailed : public std::runtime_error {
public:
    AssertionFailed(const char *file, int line, const char *func, const char *expr)
        : std::runtime_error(std::string(file != nullptr ? file : "?") + ":" +
                             std::to_string(line) + ": " +
                             (func != nullptr ? func : "?") + ": assertion failed: " +
                             (expr != nullptr ? expr : "?")),
          file_(file), line_(line), func_(func), expr_(expr) {}

    /// Source file of the failing assertion.
    [[nodiscard]] const char *file() const { return file_; }
    /// Line of the failing assertion.
    [[nodiscard]] int line() const { return line_; }
    /// Enclosing function, from @c __func__.
    [[nodiscard]] const char *function() const { return func_; }
    /// The condition as written at the call site.
    [[nodiscard]] const char *expression() const { return expr_; }

private:
    const char *file_;
    int line_;
    const char *func_;
    const char *expr_;
};

/**
 * @brief Write a failed assertion to the run's log.
 *
 * The macro throws; whether that reaches anyone depends on the caller. This is
 * the one-line form for the common case where the point is that the failure
 * appears in the run's own output, not only in whatever eventually caught it.
 */
void reportAssertion(const AssertionFailed &e);

}  // namespace ktplace

/**
 * @brief Assert a condition that must hold.
 *
 * Throws AssertionFailed rather than aborting. This is a library a caller drives,
 * so a failure that names the file, the line and the enclosing function and then
 * unwinds is worth more than a signal that ends the process with a message on a
 * stream nobody is reading. The intended use is the class of condition where
 * continuing would produce a plausible-looking wrong answer -- an empty grid, a
 * cell with no nets, a bin index past the end -- which is exactly what a silent
 * default or a wrapped-away error turns into a long debugging session.
 *
 * The function name is @c __func__, so a failure names the method it happened in
 * without the call site having to say so.
 *
 * @code
 *   KTASSERT(bins > 0);
 *   KTASSERT(index < size);
 * @endcode
 */
#define KTASSERT(cond)                                                            \
    do {                                                                          \
        if (!(cond)) {                                                            \
            throw ::ktplace::AssertionFailed(__FILE__, __LINE__, __func__, #cond); \
        }                                                                         \
    } while (false)
