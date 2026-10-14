#include "kt_flowMgr.h"
#include "kt_option.h"

#include "session/kt_session.h"
#include "tcl/kt_tclShell.h"
#include "util/kt_log.h"
#include "visualization/kt_plotOptions.h"

#include <cstdlib>
#include <exception>

using namespace ktplace;

namespace {

// `ktplace -t script.tcl`: the script in place of FlowMgr's fixed sequence. The
// session starts drawing where the fixed flow would (-w, --no-plots and the
// KTPLACE_ANIM_* parameters apply), and the script can change that with
// set_plot_dir. A failing command fails the run with Tcl's own trace, which
// names the command, the procedure and the line.
int runScript(const kt_option &options) {
    ktSession session;
    PlotOptions plot = options.plot ? PlotOptions::fromEnvironment() : PlotOptions{};
    plot.dir = options.getPlotDir();
    session.setPlotOptions(plot);

    TclShell shell(session);
    shell.setGlobal("input_dir", options.inputPath);
    shell.setGlobal("work_dir", options.workDir);
    shell.setGlobal("output", options.getOutputPath());
    shell.setGlobal("plot_dir", options.getPlotDir());
    shell.setGlobal("algorithm", options.algorithm);

    ktlog.echo("Running Tcl script {}...", options.tclScript);
    const TclShell::Result result = shell.evalFile(options.tclScript);
    if (!result.ok) {
        ktlog.fatal("Tcl script failed: {}", result.trace);
    }
    ktlog.echo("Tcl script completed successfully!");
    return EXIT_SUCCESS;
}

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

        if (!options.tclScript.empty()) {
            return runScript(options);
        }

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
