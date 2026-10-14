#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <filesystem>
#include <fmt/format.h>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ktplace {

using ktplace::ktlog;

bool kt_option::parse(int argc, char *argv[]) {
    for (int i = 0; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return false;
        }
        if (arg == "-V" || arg == "--version") {
            printVersion();
            return false;
        }
    }

    // Cleared, so a second parse describes only the second command line.
    inputPath.clear();
    tclScript.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(fmt::format("{} requires a value", arg));
            }
            return argv[++i];
        };

        if (arg == "-a" || arg == "--algorithm") {
            algorithm = value();
        } else if (arg == "-w" || arg == "--work-dir") {
            workDir = value();
        } else if (arg == "-t" || arg == "--tcl") {
            tclScript = value();
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--no-plots") {
            plot = false;
        } else if (!arg.empty() && arg.front() != '-' && inputPath.empty()) {
            inputPath = arg;
        } else {
            ktlog.echo("");
            printUsage();
            ktlog.fatal("Unknown command-line option: {}", arg);
        }
    }

    if (inputPath.empty() && tclScript.empty()) {
        ktlog.echo("");
        printUsage();
        ktlog.fatal("Insufficient command-line arguments");
    }

    std::error_code ec;
    std::filesystem::create_directories(workDir, ec);
    if (ec) {
        throw std::runtime_error("cannot create work directory '" + workDir + "': " + ec.message());
    }

    return true;
}

void kt_option::report() const {
    ktReportTable config("Configuration");
    config.add("input directory",
               inputPath.empty() ? std::string("(read by the script)") : inputPath);
    if (!tclScript.empty()) {
        config.add("tcl script", tclScript);
    }
    config.add("work directory", workDir);
    config.add("output path", getOutputPath());
    config.add("plot directory", getPlotDir());
    config.add("log file", getLogFile());
    config.add("algorithm", algorithm);
    if (verbose) {
        config.add("trace file", Logger::tracePathFor(getLogFile()));
    }
    config.emit();
}

void kt_option::printUsage() {
    constexpr const char *kUsage = R"(KTPlace - Know Thyself Placement Engine v{}
Usage: ktplace <input_dir> [options]
       ktplace -t <script.tcl> [input_dir] [options]

Arguments:
  input_dir        Directory holding the design's input files, which are named
                   after the design. Optional with -t, where the script reads
                   its own design (`read_design $ktplace(input_dir)`)

Options:
  -a, --algorithm <name>    Placement algorithm (default: simpl)
  -t, --tcl <script>        Run a Tcl script instead of the fixed flow. The
                            script sees ktplace(input_dir), ktplace(work_dir),
                            ktplace(output), ktplace(plot_dir) and
                            ktplace(algorithm); `help` lists the commands
  -w, --work-dir <dir>      Where to write everything; created if missing
                            (default: the current directory)
  --no-plots                Draw nothing; skip every frame and picture
  -v, --verbose             Also send trace diagnostics to the console.
                            <log>_trace.log is always written either way


  -h, --help                Show this help message
  -V, --version             Show version information

Everything a run writes goes under the work directory: placed.pl, plots/, and
ktplace.log. Logging goes to that file and stderr; stdout is never written to.

The picture settings -- animation on or off, frame budget, zoom levels -- are
defaults that the environment can override (KTPLACE_ANIM, KTPLACE_ANIM_MAX_FRAMES,
KTPLACE_FINAL_ZOOM, ...), so tuning them does not need a flag each.

Examples:
  ktplace benchmark/ISPD_2005/adaptec1
  ktplace benchmark/ISPD_2005/adaptec1 -w output
  ktplace benchmark/ISPD_2005/adaptec1 -w output -a simpl
  ktplace -t scripts/flow.tcl benchmark/ISPD_2005/adaptec1 -w output
)";
    ktlog.echo(fmt::format(kUsage, VERSION));
}

void kt_option::printVersion() {
    ktlog.echo(fmt::format("KTPlace v{}\nKnow Thyself Placement Engine", VERSION));
}

}  // namespace ktplace
