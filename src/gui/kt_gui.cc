// @file kt_gui.cc
// ktplace-gui: the KTPlace main window.
//
//   ktplace-gui [input_dir] [-w work_dir] [-t script.tcl] [-b bench_root] [-v]
//               [Qt options]
//
// The same arguments as ktplace, all optional: a design given here is read on
// start-up, a script is sourced after it, and -w is where the log, the
// pictures and the run history (runs.json) go. -b is the directory the
// Benchmarks panel lists designs from; without it, ./benchmark when there is
// one. -c configuration scripts are sourced before anything else. Qt's own options (-platform xcb, -style, ...)
// are taken first. Unlike ktplace, nothing runs unless asked for; the window
// waits for commands.

#include "kt_option.h"

#include "gui/kt_engine.h"
#include "gui/kt_mainWindow.h"
#include "gui/kt_tclConsole.h"
#include "util/kt_log.h"

#include <QApplication>
#include <QTimer>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace ktplace;

namespace {

struct GuiOptions {
    std::string inputPath;
    std::string script;
    std::string workDir = std::filesystem::current_path().string();
    std::string benchRoot;
    std::string algorithm = "simpl";
    std::vector<std::string> configs;
    bool verbose = false;
};

// Throws std::runtime_error on a bad command line, which main() reports.
GuiOptions parseArguments(int argc, char *argv[]) {
    GuiOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string(arg) + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "-w" || arg == "--work-dir") {
            options.workDir = value();
        } else if (arg == "-t" || arg == "--tcl") {
            options.script = value();
        } else if (arg == "-b" || arg == "--bench-root") {
            options.benchRoot = value();
        } else if (arg == "-c" || arg == "--config") {
            options.configs.push_back(value());
        } else if (arg == "-a" || arg == "--algorithm") {
            options.algorithm = value();
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--no-plots") {
            // Accepted for `ktplace --gui`, which passes its own options on: the
            // window draws nothing to disk unless a script calls set_plot_dir.
        } else if (!arg.empty() && arg.front() != '-' && options.inputPath.empty()) {
            options.inputPath = arg;
        } else {
            throw std::runtime_error("unknown command-line option: " + std::string(arg));
        }
    }
    std::filesystem::create_directories(options.workDir);
    if (options.benchRoot.empty()) {
        // -b, or the repository's own benchmark/ when run from the repository.
        if (std::error_code ec; std::filesystem::is_directory("benchmark", ec)) {
            options.benchRoot = std::filesystem::absolute("benchmark").string();
        }
    }
    return options;
}

}  // namespace

int main(int argc, char *argv[]) {
    // QApplication removes the options it recognises from argv.
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KTPlace"));
    QApplication::setApplicationVersion(
        QString::fromUtf8(VERSION.data(), static_cast<qsizetype>(VERSION.size())));

    GuiOptions options;
    try {
        options = parseArguments(argc, argv);
    } catch (const std::exception &e) {
        ktlog.echo(
            "usage: ktplace-gui [input_dir] [-w work_dir] [-c config.tcl] [-t script.tcl] [-a "
            "algorithm] [-b "
            "bench_root] [-v]");
        ktlog.warning("{}", e.what());
        ktlog.shutdown();
        return EXIT_FAILURE;
    }

    const std::filesystem::path work(options.workDir);
    ktlog.configure((work / "ktplace.log").string(), options.verbose);
    ktlog.echo("KTPlace GUI v{}", VERSION);

    int status = EXIT_SUCCESS;
    {
        Engine engine;
        engine.setGlobal("work_dir", QString::fromStdString(options.workDir));
        engine.setGlobal("input_dir", QString::fromStdString(options.inputPath));
        engine.setGlobal("output", QString::fromStdString((work / "placed.pl").string()));
        engine.setGlobal("plot_dir", QString::fromStdString((work / "plots").string()));
        engine.setGlobal("algorithm", QString::fromStdString(options.algorithm));

        MainWindow window(engine);
        window.setHistoryFile(QString::fromStdString((work / "runs.json").string()));
        window.setBenchmarkRoot(QString::fromStdString(options.benchRoot));
        // A VNC or frame-buffer display has no window manager to size the
        // window, so it takes the whole screen there.
        const QString platform = QGuiApplication::platformName();
        if (platform == "vnc" || platform == "linuxfb" || platform == "eglfs") {
            window.showMaximized();
        } else {
            window.show();
        }

        // Once the event loop runs, so the commands appear in the console as
        // they execute. One script, so the read is finished before sourcing.
        QString startup;
        // Configuration first, so the stages read what it set.
        for (const std::string &config : options.configs) {
            startup += "source " + MainWindow::tclQuote(QString::fromStdString(config)) + "\n";
        }
        if (!options.inputPath.empty()) {
            startup += "read_design " +
                       MainWindow::tclQuote(QString::fromStdString(options.inputPath)) + "\n";
        }
        if (!options.script.empty()) {
            startup +=
                "source " + MainWindow::tclQuote(QString::fromStdString(options.script)) + "\n";
        }
        if (!startup.isEmpty()) {
            QTimer::singleShot(0, &window, [&window, startup] {
                window.console()->run(startup.trimmed());
            });
        }
        status = QApplication::exec();
    }
    ktlog.shutdown();
    return status;
}
