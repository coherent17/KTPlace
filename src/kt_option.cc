#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_platform.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ktplace {

namespace {

constexpr std::array<std::string_view, 3> modeNames = {"batch", "shell", "gui"};

bool isAnyOf(std::string_view arg, std::string_view shortName, std::string_view longName) {
    return arg == shortName || arg == longName;
}

[[noreturn]] void usageError(std::string_view message) {
    ktlog.echo("");
    kt_option::printUsage();
    ktlog.fatal(message);
}

}  // namespace

bool kt_option::parse(int argc, char *argv[]) {
    const std::vector<std::string_view> all(argv + 1, argv + argc);
    command = std::accumulate(all.begin(), all.end(), std::string(argv[0]),
                              [](std::string line, std::string_view arg) {
                                  return std::move(line) + ' ' + std::string(arg);
                              });
    if (std::ranges::any_of(all, [](std::string_view arg) {
            return isAnyOf(arg, "-h", "--help");
        })) {
        printUsage();
        return false;
    }
    if (std::ranges::any_of(all, [](std::string_view arg) {
            return isAnyOf(arg, "-V", "--version");
        })) {
        printVersion();
        return false;
    }

    std::vector<std::string_view> args;
    for (const std::string_view arg : all) {
        if (arg == "--gui" || arg == "--shell") {
            const Mode asked = arg == "--gui" ? Mode::Gui : Mode::Shell;
            if (mode != Mode::Batch && mode != asked) {
                usageError("--gui and --shell cannot be combined");
            }
            mode = asked;
        } else {
            args.push_back(arg);
        }
    }

    auto next = args.begin();
    if (next != args.end() && !next->starts_with('-')) {
        inputPath = *next++;
    }

    while (next != args.end()) {
        const std::string_view arg = *next++;
        const auto value = [&] {
            if (next == args.end()) {
                throw std::runtime_error(std::format("{} requires a value", arg));
            }
            return std::string(*next++);
        };

        if (isAnyOf(arg, "-w", "--work-dir")) {
            workDir = value();
        } else if (isAnyOf(arg, "-v", "--verbose")) {
            verbose = true;
        } else if (isAnyOf(arg, "-tcl", "--tcl")) {
            tclFile = value();
        } else {
            usageError(std::format("Unknown command-line option: {}", arg));
        }
    }

    if (mode == Mode::Batch && inputPath.empty() && tclFile.empty()) {
        usageError("Insufficient command-line arguments");
    }

    std::error_code ec;
    std::filesystem::create_directories(workDir, ec);
    if (ec) {
        throw std::runtime_error(
            std::format("cannot create work directory '{}': {}", workDir, ec.message()));
    }
    return true;
}

void kt_option::report() const {
    ktlog.echo("KTPlace - Know Thyself Placement Engine v{}, started {}", version, localDateTime());
    PlatformInfo::detect().report();

    ktReportTable config("Configuration");
    if (!command.empty()) {
        config.add("command", command);
    }
    config.add("mode", std::string(modeNames[std::to_underlying(mode)]));
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
       ktplace [<input_dir>] -tcl <flow.tcl> [options]
       ktplace --shell [<input_dir>] [options]
       ktplace --gui [<input_dir>] [options]

Arguments:
  input_dir        Directory holding the design's input files, which are named
                   after the design

Options:
  -w, --work-dir <dir>      Where to write everything; created if missing
                            (default: the current directory)
  -tcl, --tcl <file>        Tcl script for the run: add_option lines configure
                            the default flow; read_design, gplace_design,
                            legalize_placement, detail_place, write_placement
                            ... make the script the flow itself
  --shell                   Interactive Tcl console (prompt on stderr)
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
tuning a run does not need a flag each; `add_option visualizer plot 0` draws
nothing at all.

Examples:
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1 -w output
  ktplace benchmark/bookshelf/ISPD_2005/adaptec1 -w output -tcl run.tcl
  ktplace -w output -tcl flow.tcl
  ktplace --shell benchmark/bookshelf/ICCAD04/ibm01 -w output
)";
    ktlog.echo(std::format(usage, version));
}

void kt_option::printVersion() {
    ktlog.echo(std::format("KTPlace v{}\nKnow Thyself Placement Engine", version));
}

}  // namespace ktplace
