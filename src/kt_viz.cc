// @file kt_viz.cc
// ktplace-viz: draw a run's pictures from the trace it recorded
//
// `ktplace --record` writes plots/trace.ktrace instead of drawing. This draws
// it: the same SVG frames, density maps, bounds curve, GIF and final still the
// run would have drawn, into the same layout. Only the visualizer and the data
// it draws are linked in -- no placer, legalizer or reader.

#include "kt_option.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_trace.h"
#include "visualization/kt_visualizer.h"

#include <exception>
#include <filesystem>
#include <fmt/format.h>
#include <optional>
#include <string>
#include <string_view>

using namespace ktplace;

namespace {

void printUsage() {
    ktlog.echo(fmt::format(R"(ktplace-viz - draw a recorded ktplace run v{}
Usage: ktplace-viz <trace> [options]

Arguments:
  trace                    A trace written by `ktplace --record`
                           (<work-dir>/plots/trace.ktrace)

Options:
  -o, --out <dir>          Where to draw (default: the trace's own directory,
                           which is the layout the run would have drawn)
  --no-anim                Skip the animation; draw the frames and stills only
  --final-zoom <z>         Scale of the final still against 768x768
  -v, --verbose            Also send trace diagnostics to the console
  -h, --help               Show this help message
  -V, --version            Show version information

The picture settings default to the ones the run recorded.
)",
                           VERSION));
}

struct VizOptions {
    std::string trace;
    std::string outDir;
    bool noAnim = false;
    std::optional<double> finalZoom;
    bool verbose = false;
};

/// False when the run should stop without drawing (help, version).
bool parse(int argc, char *argv[], VizOptions &o) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                ktlog.fatal("{} requires a value", arg);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return false;
        } else if (arg == "-V" || arg == "--version") {
            ktlog.echo(fmt::format("ktplace-viz v{}", VERSION));
            return false;
        } else if (arg == "-o" || arg == "--out") {
            o.outDir = value();
        } else if (arg == "--no-anim") {
            o.noAnim = true;
        } else if (arg == "--final-zoom") {
            const std::string z = value();
            try {
                o.finalZoom = std::stod(z);
            } catch (const std::exception &) {
                ktlog.fatal("--final-zoom takes a number, not '{}'", z);
            }
            if (!(*o.finalZoom > 0.0)) {
                ktlog.fatal("--final-zoom must be positive");
            }
        } else if (arg == "-v" || arg == "--verbose") {
            o.verbose = true;
        } else if (!arg.empty() && arg.front() == '-') {
            printUsage();
            ktlog.fatal("Unknown command-line option: {}", arg);
        } else if (o.trace.empty()) {
            o.trace = arg;
        } else {
            ktlog.fatal("one trace at a time (got '{}' and '{}')", o.trace, arg);
        }
    }
    if (o.trace.empty()) {
        printUsage();
        ktlog.fatal("no trace given");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(o.trace, ec)) {
        ktlog.fatal("cannot read trace {}", o.trace);
    }
    if (o.outDir.empty()) {
        o.outDir = std::filesystem::path(o.trace).parent_path().string();
        if (o.outDir.empty()) {
            o.outDir = ".";
        }
    }
    std::filesystem::create_directories(o.outDir, ec);
    if (ec) {
        ktlog.fatal("cannot create {}: {}", o.outDir, ec.message());
    }
    return true;
}

}  // namespace

int main(int argc, char *argv[]) {
    try {
        VizOptions o;
        if (!parse(argc, argv, o)) {
            ktlog.shutdown();
            return 0;
        }
        ktlog.configure((std::filesystem::path(o.outDir) / "ktplace-viz.log").string(), o.verbose);

        const TraceReplay r =
            replayTrace(o.trace, o.outDir, Visualizer::instance(), [&o](PlotOptions &p) {
                if (o.noAnim) {
                    p.animate = false;
                }
                if (o.finalZoom) {
                    p.finalZoom = *o.finalZoom;
                }
            });

        ktReportTable table("Trace drawn");
        table.add("trace", o.trace);
        table.add("drawn into", o.outDir);
        table.add("snapshots", fmt::format("{}", r.frames));
        table.add("animation", r.animationWritten ? "anim/placement.gif" : "none");
        table.add("complete run", r.complete ? "yes" : "no (the run stopped early)");
        table.emit();
        TimerRegistry::instance().report();
        ktlog.shutdown();
        return 0;

    } catch (const std::exception &e) {
        ktlog.fatal("Error: {}", e.what());
    } catch (...) {
        ktlog.fatal("Error: unknown exception");
    }
}
