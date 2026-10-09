#include "kt_flowMgr.h"
#include "kt_option.h"

#include "util/kt_log.h"

#include <exception>

using namespace ktplace;

int main(int argc, char *argv[]) {
    try {
        kt_option options;
        if (!options.parse(argc, argv)) {
            ktlog.shutdown();
            return 0;
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
        ktlog.shutdown();
        return 0;

    } catch (const std::exception &e) {
        ktlog.fatal("Error: {}", e.what());
    } catch (...) {
        ktlog.fatal("Error: unknown exception");
    }
}
