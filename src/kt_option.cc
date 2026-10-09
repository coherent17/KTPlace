#include "kt_option.h"

#include "util/kt_log.h"

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

    if (argc < 2) {
        ktlog.echo("");
        printUsage();
        ktlog.fatal("Insufficient command-line arguments");
    }

    inputPath = argv[1];

    for (int i = 2; i < argc; ++i) {
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
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
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

void kt_option::printUsage() {
    constexpr const char *kUsage = R"(KTPlace - Know Thyself Placement Engine v{}
Usage: ktplace <input_dir> [options]

Arguments:
  input_dir        Directory holding the design's input files, which are named
                   after the design

Options:
  -a, --algorithm <name>    Placement algorithm (default: simpl)
  -w, --work-dir <dir>      Where to write everything; created if missing
                            (default: the current directory)
  -v, --verbose             Also send trace diagnostics to the console.
                            <log>_trace.log is always written either way.
  -h, --help                Show this help message
  -V, --version             Show version information

Everything a run writes goes under the work directory: placed.pl, plots/, and
ktplace.log. Logging goes to that file and stderr; stdout is never written to.

Examples:
  ktplace benchmark/ISPD_2005/adaptec1
  ktplace benchmark/ISPD_2005/adaptec1 -w output
  ktplace benchmark/ISPD_2005/adaptec1 -w output -a simpl
)";
    ktlog.echo(fmt::format(kUsage, VERSION));
}

void kt_option::printVersion() {
    ktlog.echo(fmt::format("KTPlace v{}\nKnow Thyself Placement Engine", VERSION));
}

}  // namespace ktplace
