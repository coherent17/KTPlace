/**
 * @file kt_assert.cc
 * @brief Out-of-line pieces of the assertion facility.
 *
 * The macro and the exception need no code of their own; what lives here is the
 * one thing that is worth having somewhere to call, which is turning a failure
 * into a line of log so an assertion thrown deep inside a solve is visible in the
 * run's own output and not only in whatever caught it.
 */

#include "util/kt_assert.h"

#include "util/kt_log.h"

namespace ktplace {

void reportAssertion(const AssertionFailed &e) {
    // echo, not warning: the caller decides whether this is a defect or a
    // legitimate rejection of an input, and the assertion is the fact, not the
    // judgement.
    ktlog.echo("assertion failed: {} ({}:{} in {})", e.what(), e.file(), e.line(),
               e.function() != nullptr ? e.function() : "?");
}

}  // namespace ktplace
