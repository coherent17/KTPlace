/**
 * @file kt_place.cc
 * @brief Main entry point for KTPlace placement engine
 */

#include "kt_place.h"
#include "kt_flowMgr.h"
#include "kt_option.h"
#include "util/kt_log.h"
#include <stdexcept>

using namespace ktplace;
using namespace ktplace::core;
using ktplace::ktlog;

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
        ktlog.echo("Input base name:  {}", options.getInputBaseName());
        ktlog.echo("Input directory:  {}", options.getInputDir());
        ktlog.echo("Output path:      {}", options.getOutputPath());
        ktlog.echo("Algorithm:        {}", options.getAlgorithm());
        ktlog.echo("Output format:    {}", options.getOutputFormat());
        if (!options.getLogFile().empty()) {
            ktlog.echo("Log file:         {}", options.getLogFile());
        }
        if (!options.getConfigFile().empty()) {
            ktlog.echo("Config file:      {}", options.getConfigFile());
        }
        if (!options.getPlotDir().empty()) {
            ktlog.echo("Plot directory:   {}", options.getPlotDir());
        }
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
