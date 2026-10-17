#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--gui") {
            gui = true;
        } else {
            args.emplace_back(argv[i]);
        }
    }

    std::size_t first = 0;
    if (!args.empty() && !(gui && args.front().starts_with('-'))) {
        inputPath = args.front();
        first = 1;
    } else if (!gui) {
        ktlog.echo("");
        printUsage();
        ktlog.fatal("Insufficient command-line arguments");
    }

    for (std::size_t i = first; i < args.size(); ++i) {
        const std::string &arg = args[i];

        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) {
                throw std::runtime_error(std::format("{} requires a value", arg));
            }
            return args[++i];
        };

        if (arg == "-w" || arg == "--work-dir") {
            workDir = value();
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg == "--no-plots") {
            plot = false;
        } else if (arg == "-tcl" || arg == "--tcl") {
            tclFile = value();
        } else {
            ktlog.echo("");
            printUsage();
            ktlog.fatal("Unknown command-line option: {}", arg);
        }
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
    config.add("input directory", inputPath);
    config.add("work directory", workDir);
    config.add("output path", getOutputPath());
    config.add("plot directory", getPlotDir());
    config.add("log file", getLogFile());
    if (verbose) {
        config.add("trace file", Logger::tracePathFor(getLogFile()));
    }
    config.emit();
}

void kt_option::printUsage() {
    constexpr const char *usage = R"(KTPlace - Know Thyself Placement Engine v{}
Usage: ktplace <input_dir> [options]
       ktplace --gui [<input_dir>] [options]

Arguments:
  input_dir        Directory holding the design's input files, which are named
                   after the design

Options:
  -w, --work-dir <dir>      Where to write everything; created if missing
                            (default: the current directory)
  --no-plots                Draw nothing; skip every frame and picture
  -tcl, --tcl <file>        Tcl script configuring the run, through
                            add_option <component> <key> <value>
  -v, --verbose             Also send trace diagnostics to the console.
                            <log>_trace.log is always written either way
  --gui                     Open the Qt window instead, placing <input_dir> if
                            given (needs a build with make GUI=1)


  -h, --help                Show this help message
  -V, --version             Show version information

Everything a run writes goes under the work directory: placed.pl, plots/, and
ktplace.log. Logging goes to that file and stderr; stdout is never written to.

Everything a run is made of is chosen in that script, including the placer:
`add_option flow algorithm ntuplace1`, where simpl is the default. The picture
settings -- animation on or off, frame budget, zoom levels -- are options too, so
tuning a run does not need a flag each; `add_option visualizer anim 0` is the
-tcl spelling of --no-plots.

Examples:
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1 -w output
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1 -w output -tcl run.tcl
)";
    ktlog.echo(std::format(usage, version));
}

void kt_option::printVersion() {
    ktlog.echo(std::format("KTPlace v{}\nKnow Thyself Placement Engine", version));
}

}  // namespace ktplace
