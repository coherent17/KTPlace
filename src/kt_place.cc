#include "kt_flowMgr.h"
#include "kt_option.h"

#include "util/kt_log.h"

#include <cstdlib>
#include <exception>

using namespace ktplace;

namespace {

// The whole CLI run. Anything other than a FatalError that escapes is turned
// into one here, so main() has a single way to fail.
int run(int argc, char *argv[]) {
    try {
        kt_option options;
        if (!options.parse(argc, argv)) {
            return EXIT_SUCCESS;
        }

        ktlog.configure(options.getLogFile(), options.verbose);
        ktlog.trace("ktplace start: argc={}", argc);
        ktlog.echo("KTPlace - Know Thyself Placement Engine v{}", VERSION);
        ktlog.echo("========================================");
        options.report();

        FlowMgr flowMgr;
        ktlog.echo("Starting placement flow...");
        flowMgr.run(options);
        ktlog.echo("Placement completed successfully!");
        ktlog.echo("Results written to: {}", options.getOutputPath());
        return EXIT_SUCCESS;

    } catch (const FatalError &) {
        throw;  // already logged where it was raised; catch (...) must not re-log it
    } catch (const std::exception &e) {
        ktlog.fatal("Error: {}", e.what());
    } catch (...) {
        ktlog.fatal("Error: unknown exception");
    }
}

}  // namespace

int main(int argc, char *argv[]) {
    int status = EXIT_FAILURE;
    try {
        status = run(argc, argv);
    } catch (const FatalError &) {
        // ktlog.fatal wrote the record before throwing; nothing more to say.
    }
    ktlog.shutdown();
    return status;
}
