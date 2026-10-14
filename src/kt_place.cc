#include "kt_flowMgr.h"
#include "kt_option.h"

#include "session/kt_session.h"
#include "tcl/kt_tcl.h"
#include "tcl/kt_tclMgr.h"
#include "tcl/kt_tclShell.h"
#include "util/kt_log.h"
#include "visualization/kt_plotOptions.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

using namespace ktplace;

namespace {

// `ktplace -t script.tcl`: the script in place of FlowMgr's fixed sequence. The
// session starts drawing where the fixed flow would (-w, --no-plots and the
// visualizer options apply), and the script can change that with
// set_plot_dir. A failing command fails the run with Tcl's own trace, which
// names the command, the procedure and the line.
int runScript(const kt_option &options) {
    ktSession session;
    PlotOptions plot = options.plot ? PlotOptions::fromOptions() : PlotOptions{};
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

// `ktplace --gui ...`: become ktplace-gui, with the other arguments.
//
// The CLI links no Qt, so the window is its own binary; this only finds it and
// execs it, and from then on the process is the GUI. Only a failure to get
// there comes back, reported on stderr (the log is not open yet).
int launchGui(const char *argv0, const std::vector<std::string> &args) {
    const std::filesystem::path gui = kt_option::guiExecutable(argv0);
    if (std::error_code ec; !std::filesystem::exists(gui, ec)) {
        ktlog.warning("ktplace-gui is not built ({} does not exist); build it with `make gui`",
                      gui.string());
        return EXIT_FAILURE;
    }
    // Qt would fail with a longer message and an abort; say what to do instead.
    const auto set = [](const char *name) {
        const char *v = std::getenv(name);
        return v != nullptr && *v != '\0';
    };
    const bool platformArg = std::ranges::find(args, "-platform") != args.end();
    if (!set("DISPLAY") && !set("WAYLAND_DISPLAY") && !set("QT_QPA_PLATFORM") && !platformArg) {
        ktlog.warning(
            "--gui: no display to open the window on (DISPLAY is not set). The dev "
            "container has one at :1, served over VNC; from the host, use "
            "scripts/devenv.sh gui");
        return EXIT_FAILURE;
    }
    std::vector<std::string> words;
    words.push_back(gui.string());
    words.insert(words.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (std::string &w : words) {
        argv.push_back(w.data());
    }
    argv.push_back(nullptr);
    ::execv(gui.c_str(), argv.data());
    ktlog.warning("cannot start {}: {}", gui.string(), std::strerror(errno));
    return EXIT_FAILURE;
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

        // The configuration first, so every stage below reads what it set.
        for (const std::string &script : options.configScripts) {
            ktlog.echo("Configuring from {}...", script);
            try {
                tcl::TclMgr::instance().loadScript(script);
            } catch (const tcl::Error &e) {
                ktlog.fatal("configuration script {} failed: {}", script, e.what());
            }
        }

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
    if (const auto gui = kt_option::guiArguments(argc, argv)) {
        const int failed = launchGui(argc > 0 ? argv[0] : nullptr, *gui);
        ktlog.shutdown();
        return failed;
    }
    int status = EXIT_FAILURE;
    try {
        status = run(argc, argv);
    } catch (const FatalError &) {
        // ktlog.fatal wrote the record before throwing; nothing more to say.
    }
    ktlog.shutdown();
    return status;
}
