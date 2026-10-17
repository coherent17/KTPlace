#include "kt_flowMgr.h"
#include "kt_option.h"

#include "gui/kt_gui.h"
#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <chrono>
#include <ctime>
#include <exception>
#include <string>

using namespace ktplace;

int main(int argc, char *argv[]) {
    const auto started = std::chrono::steady_clock::now();
    const std::clock_t cpuStarted = std::clock();
    try {
        kt_option options;
        if (!options.parse(argc, argv)) {
            ktlog.shutdown();
            return 0;
        }
        if (options.gui) {
            return runGui(options, argc, argv);
        }

        ktlog.configure(options.getLogFile(), options.verbose);
        ktlog.trace("ktplace start: argc={}", argc);
        ktlog.beginStep("setup");
        ktlog.echo("KTPlace - Know Thyself Placement Engine v{}", version);
        std::string command = argv[0];
        for (int i = 1; i < argc; ++i) {
            command += ' ';
            command += argv[i];
        }
        ktlog.echo("Run started: {}", localDateTime());
        ktlog.echo("Command: {}", command);
        options.report();

        FlowMgr flowMgr;
        flowMgr.run(options);
        ktlog.echo("Placement completed successfully!");
        ktlog.echo("Results written to: {}", options.getOutputPath());
        const double wall =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        const double cpu =
            static_cast<double>(std::clock() - cpuStarted) / static_cast<double>(CLOCKS_PER_SEC);
        ktlog.echo("Run finished: {}; total {:.3f}s wall, {:.3f}s cpu, peak memory {:.1f} MiB",
                   localDateTime(), wall, cpu,
                   static_cast<double>(peakResidentBytes()) / (1024.0 * 1024.0));
        ktlog.shutdown();
        return 0;

    } catch (const std::exception &e) {
        ktlog.fatal("Error: {}", e.what());
    } catch (...) {
        ktlog.fatal("Error: unknown exception");
    }
}
