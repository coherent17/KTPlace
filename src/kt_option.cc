// @file kt_option.cc// Implementation of command-line option parser


#include "kt_option.h"
#include "util/kt_log.h"
#include <fmt/format.h>
#include <string>
#include <filesystem>
#include <stdexcept>
#include <cstring>

namespace ktplace {

using ktplace::ktlog;

// Version information
const char *VERSION = "0.1.0";

// PIMPL implementation
class kt_option::Impl {
public:
    // Parsed options
    std::string inputBaseName;
    std::string inputDir;
    std::string outputPath;
    std::string algorithm = "simpl";
    std::string outputFormat = "bookshelf";
    std::string logFile;
    bool verbose = false;
    std::string configFile;
    std::string plotDir;
    std::string workDir;

    // Flags
    bool helpRequested = false;
    bool versionRequested = false;
};

// kt_option implementation

kt_option::kt_option() : pImpl(std::make_unique<Impl>()) {}

kt_option::~kt_option() = default;

kt_option::kt_option(kt_option &&) noexcept = default;
kt_option &kt_option::operator=(kt_option &&) noexcept = default;

bool kt_option::parse_option(int argc, char *argv[]) {
    // Reset flags
    pImpl->helpRequested = false;
    pImpl->versionRequested = false;

    // Check for help or version flags first
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            pImpl->helpRequested = true;
            printUsage(argv[0]);
            return false;  // Indicate help was shown
        }
        if (arg == "-V" || arg == "--version") {
            pImpl->versionRequested = true;
            printVersion();
            return false;  // Indicate version was shown
        }
    }

    // Require at least 3 arguments (base name, input dir, output path)
    if (argc < 4) {
        ktlog.echo("");
        printUsage(argv[0]);
        ktlog.fatal("Insufficient command-line arguments");
        throw std::runtime_error("Insufficient command-line arguments");
    }

    // Parse positional arguments
    pImpl->inputBaseName = argv[1];
    pImpl->inputDir = argv[2];
    pImpl->outputPath = argv[3];

    // Parse optional arguments
    for (int i = 4; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-a" || arg == "--algorithm") {
            if (i + 1 < argc) {
                pImpl->algorithm = argv[++i];
            } else {
                throw std::runtime_error("--algorithm requires a value");
            }
        } else if (arg == "-f" || arg == "--format") {
            if (i + 1 < argc) {
                pImpl->outputFormat = argv[++i];
            } else {
                throw std::runtime_error("--format requires a value");
            }
        } else if (arg == "-l" || arg == "--log") {
            if (i + 1 < argc) {
                pImpl->logFile = argv[++i];
            } else {
                throw std::runtime_error("--log requires a value");
            }
        } else if (arg == "-w" || arg == "--work-dir") {
            if (i + 1 < argc) {
                pImpl->workDir = argv[++i];
            } else {
                throw std::runtime_error("--work-dir requires a value");
            }
        } else if (arg == "-v" || arg == "--verbose") {
            pImpl->verbose = true;
        } else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) {
                pImpl->configFile = argv[++i];
            } else {
                throw std::runtime_error("--config requires a value");
            }
        } else if (arg == "-p" || arg == "--plot") {
            if (i + 1 < argc) {
                pImpl->plotDir = argv[++i];
            } else {
                throw std::runtime_error("--plot requires a value");
            }
        } else {
            ktlog.echo("");
            printUsage(argv[0]);
            ktlog.fatal("Unknown command-line option: {}", arg);
        }
    }

    resolvePaths();
    return true;  // Success
}

namespace {

/// True for paths that already name their location (POSIX and Windows forms).
bool isAbsolute(const std::string &path) {
    return !path.empty() &&
           (path.front() == '/' || path.front() == '\\' || (path.size() > 2 && path[1] == ':'));
}

}  // namespace

void kt_option::resolvePaths() {
    if (pImpl->workDir.empty()) {
        // No work directory: keep whatever the user typed.
        if (pImpl->logFile.empty()) {
            pImpl->logFile = "ktplace.log";
        }
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(pImpl->workDir, ec);
    if (ec) {
        throw std::runtime_error("cannot create work directory '" + pImpl->workDir +
                                 "': " + ec.message());
    }

    // Relative artifact paths live under the work directory; absolute ones are
    // honoured exactly as given.
    const auto rebase = [this](std::string &path) {
        if (!path.empty() && !isAbsolute(path)) {
            path = (std::filesystem::path(pImpl->workDir) / path).string();
        }
    };
    rebase(pImpl->outputPath);
    rebase(pImpl->plotDir);

    if (pImpl->logFile.empty()) {
        pImpl->logFile = (std::filesystem::path(pImpl->workDir) / "ktplace.log").string();
    } else {
        rebase(pImpl->logFile);
    }
}

const std::string &kt_option::getInputBaseName() const {
    return pImpl->inputBaseName;
}

const std::string &kt_option::getInputDir() const {
    return pImpl->inputDir;
}

const std::string &kt_option::getOutputPath() const {
    return pImpl->outputPath;
}

const std::string &kt_option::getAlgorithm() const {
    return pImpl->algorithm;
}

const std::string &kt_option::getOutputFormat() const {
    return pImpl->outputFormat;
}

const std::string &kt_option::getLogFile() const {
    return pImpl->logFile;
}

bool kt_option::isVerbose() const {
    return pImpl->verbose;
}

const std::string &kt_option::getConfigFile() const {
    return pImpl->configFile;
}

const std::string &kt_option::getWorkDir() const {
    return pImpl->workDir;
}

const std::string &kt_option::getPlotDir() const {
    return pImpl->plotDir;
}

void kt_option::printUsage(const char *programName) {
    constexpr const char *kUsage = R"(KTPlace - Know Thyself Placement Engine v{}
Usage: {} <input_base_name> <input_dir> <output_path> [options]

Arguments:
  input_base_name  Base name of input files (e.g., 'adaptec2')
  input_dir        Directory containing input files
  output_path      Output file path for placement results

Options:
  -a, --algorithm <name>    Placement algorithm: simpl, ntuplace1 (default: simpl)
  -f, --format <format>     Output format (default: bookshelf)
  -l, --log <file>          Transcript log file (default: ktplace.log,
                            or <work-dir>/ktplace.log with -w)
  -v, --verbose             Also send trace diagnostics to the console.
                            <log>_trace.log is always written either way.
  -w, --work-dir <dir>      Base directory for relative output, plot and log
                            paths; created if missing
  -c, --config <file>       Configuration file path (optional)
  -p, --plot <dir>          Write SVG frames, HPWL curve and an HTML gallery
                            of the solve into this directory (optional)
  -h, --help                Show this help message
  -V, --version             Show version information

Logging goes to the log file and stderr; stdout is never written to.
Trace records go to a separate <log>_trace.log, which is always written; -v
additionally sends them to the console. Keeping them out of the main log
means the main transcript stays readable.

Examples:
  {} adaptec2 ./benchmark/ISPD_2005/adaptec2 ./output/adaptec2.pl
  {} adaptec2 ./benchmark/ISPD_2005/adaptec2 ./output/adaptec2.pl -a simpl -f bookshelf
  {} adaptec2 ./benchmark/ISPD_2005/adaptec2 ./output/adaptec2.pl -l placement.log
)";
    ktlog.echo(fmt::format(kUsage, VERSION, programName, programName, programName, programName));
}

void kt_option::printVersion() {
    ktlog.echo(fmt::format("KTPlace v{}\nKnow Thyself Placement Engine", VERSION));
}

}  // namespace ktplace
