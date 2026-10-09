#include "util/kt_assert.h"

#include "util/kt_log.h"

namespace ktplace {

void reportAssertion(const AssertionFailed &e) {
    ktlog.echo("assertion failed: {} ({}:{} in {})", e.what(), e.file(), e.line(),
               e.function() != nullptr ? e.function() : "?");
}

}  // namespace ktplace
