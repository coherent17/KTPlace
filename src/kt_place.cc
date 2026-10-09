#include "kt_flowMgr.h"
#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

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

        ktlog.echo("KTPlace - Know Thyself Placement Engine");
        ktlog.echo("========================================");
        ktReportTable config("Configuration");
        config.add("input directory", options.inputPath);
        config.add("work directory", options.workDir);
        config.add("output path", options.getOutputPath());
        config.add("plot directory", options.getPlotDir());
        config.add("log file", options.getLogFile());
        config.add("algorithm", options.algorithm);
        if (ktlog.verbose()) {
            config.add("trace file", ktlog.traceFilePath());
        }
        config.emit();
        ktlog.echo("");

        FlowMgr flowMgr;

        ktlog.echo("Starting placement flow...");
        flowMgr.run(options.inputPath, options.getOutputPath(), options.algorithm,
                    options.getPlotDir());

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
        ktlog.fatal("Error: unknown exception");
    }
}