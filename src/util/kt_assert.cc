#include "util/kt_assert.h"

#include "util/kt_log.h"

namespace ktplace {

void reportAssertion(const AssertionFailed &e) {
    ktlog.echo("assertion failed: {} ({}:{} in {})", e.what(), e.getFile(), e.getLine(),
               e.getFunction() != nullptr ? e.getFunction() : "?");
}

}  // namespace ktplace
