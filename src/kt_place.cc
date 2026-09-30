// @file kt_place.cc// Main entry point for KTPlace placement engine// The entry point has no public interface, so it deliberately has no// matching header.


#include "kt_flowMgr.h"
#include "kt_option.h"
#include "util/kt_reportTable.h"
#include "util/kt_log.h"
#include <stdexcept>

using namespace ktplace;

// Main entry point
int main(int argc, char *argv[]) {
    try {
        // Parse command-line options
        kt_option options;
        if (!options.parse_option(argc, argv)) {
            // Help or version was shown
            ktlog.shutdown();
            return 0;
        }

        // Route the whole run through the logger: a transcript file plus
        // stderr, never stdout.
        ktlog.configure(options.getLogFile(), options.isVerbose());
        ktlog.trace("ktplace start: argc={}", argc);

        // Echo the effective configuration
        ktlog.echo("KTPlace - Know Thyself Placement Engine");
        ktlog.echo("========================================");
        ktReportTable config("Configuration");
        config.add("input base name", options.getInputBaseName());
        config.add("input directory", options.getInputDir());
        config.add("output path", options.getOutputPath());
        config.add("algorithm", options.getAlgorithm());
        config.add("output format", options.getOutputFormat());
        if (!options.getWorkDir().empty()) {
            config.add("work dir", options.getWorkDir());
        }
        if (!options.getLogFile().empty()) {
            config.add("log file", options.getLogFile());
        }
        if (ktlog.verbose()) {
            config.add("trace file", ktlog.traceFilePath());
        }
        if (!options.getConfigFile().empty()) {
            config.add("config file", options.getConfigFile());
        }
        if (!options.getPlotDir().empty()) {
            config.add("plot directory", options.getPlotDir());
        }
        config.emit();
        ktlog.echo("");

        // Create flow manager and run placement
        FlowMgr flowMgr;

        ktlog.echo("Starting placement flow...");
        flowMgr.run(options.getInputBaseName(), options.getInputDir(), options.getOutputPath(),
                    options.getAlgorithm(), options.getOutputFormat(), options.getPlotDir());

        ktlog.echo("");
        ktlog.echo("Placement completed successfully!");
        ktlog.echo("Results written to: {}", options.getOutputPath());

        ktlog.trace("ktplace finished normally: {} records", ktlog.recordCount());
        ktlog.shutdown();
        return 0;

    } catch (const std::exception &e) {
        ktlog.echo("");
        ktlog.fatal("Error: {}", e.what());
    } catch (...) {
        ktlog.echo("");
        ktlog.fatal("Error: Unknown exception occurred");
    }
}
