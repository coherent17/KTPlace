#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <filesystem>
#include <fmt/format.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace ktplace {

using ktplace::ktlog;

bool kt_option::parse(int argc, char *argv[], bool inputOptional) {
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
    script.clear();
    tclFile.clear();

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(fmt::format("{} requires a value", arg));
            }
            return argv[++i];
        };

        if (arg == "-w" || arg == "--work-dir") {
            workDir = value();
        } else if (arg == "-tcl" || arg == "--tcl") {
            tclFile = value();
        } else if (arg == "-s" || arg == "--script") {
            script = value();
        } else if (arg == "-b" || arg == "--bench-root") {
            benchRoot = value();
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

    if (inputPath.empty() && script.empty() && !inputOptional) {
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
    if (!tclFile.empty()) {
        config.add("tcl configuration", tclFile);
    }
    if (!script.empty()) {
        config.add("flow script", script);
    }
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
    constexpr const char *kUsage = R"(KTPlace - Know Thyself Placement Engine v{}
Usage: ktplace <input_dir> [options]
       ktplace -t <script.tcl> [input_dir] [options]

Arguments:
  input_dir        Directory holding the design's input files, which are named
                   after the design. Optional with -t, where the script reads
                   its own design (`read_design $ktplace(input_dir)`)

Options:
  -w, --work-dir <dir>      Where to write everything; created if missing
                            (default: the current directory)
  --no-plots                Draw nothing; skip every frame and picture
  -tcl, --tcl <file>        Tcl script configuring the run, through
                            add_option <component> <key> <value>
  -s, --script <file>       Run a Tcl flow script instead of the fixed flow,
                            after the --tcl configuration. The script sees
                            ktplace(input_dir), ktplace(work_dir),
                            ktplace(output), ktplace(plot_dir) and
                            ktplace(algorithm); `help` lists the commands
  -b, --bench-root <dir>    With --gui: where the Runs panel finds designs
  --gui                     Open the design in ktplace-gui, the Qt window,
                            instead of running the flow. The other arguments
                            go to it (input_dir, -w, --tcl, --script, -v);
                            `make gui` builds it. Needs a display: in the dev
                            container that is the VNC one (see docs/docker.md)
  -v, --verbose             Also send trace diagnostics to the console.
                            <log>_trace.log is always written either way


  -h, --help                Show this help message
  -V, --version             Show version information

Everything a run writes goes under the work directory: placed.pl, plots/, and
ktplace.log. Logging goes to that file and stderr; stdout is never written to.

Everything a run is made of is chosen in that script, including the placer:
`add_option flow algorithm ntuplace1`, where simpl is the default. The picture
settings -- animation on or off, frame budget, zoom levels -- are options too, so
tuning a run does not need a flag each; `add_option visualizer anim 0` is the
-tcl spelling of --no-plots. Each stage logs its full option table when it runs,
with what set each value.

Examples:
  ktplace benchmark/ISPD_2005/adaptec1
  ktplace benchmark/ISPD_2005/adaptec1 -w output
  ktplace benchmark/ISPD_2005/adaptec1 -w output -tcl run.tcl
  ktplace --script scripts/flow.tcl benchmark/ISPD_2005/adaptec1 -w output
  ktplace --gui benchmark/ISPD_2005/adaptec1 -w output
)";
    ktlog.echo(fmt::format(kUsage, VERSION));
}

std::optional<std::vector<std::string>> kt_option::guiArguments(int argc, char *argv[]) {
    bool gui = false;
    std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help" || arg == "-V" || arg == "--version") {
            return std::nullopt;
        }
        if (arg == "--gui") {
            gui = true;
        } else {
            rest.emplace_back(arg);
        }
    }
    if (!gui) {
        return std::nullopt;
    }
    return rest;
}

std::filesystem::path kt_option::guiExecutable(const char *argv0) {
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec || self.empty()) {
        // Not Linux, or no /proc: argv[0], which a shell gives as it was typed
        // or as PATH resolved it.
        self = std::filesystem::absolute(argv0 != nullptr ? argv0 : "ktplace", ec);
    }
    return self.parent_path() / "ktplace-gui";
}

void kt_option::printVersion() {
    ktlog.echo(fmt::format("KTPlace v{}\nKnow Thyself Placement Engine", VERSION));
}

}  // namespace ktplace
