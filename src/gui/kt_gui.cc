// @file kt_gui.cc
// ktplace-gui: the KTPlace main window.
//
//   ktplace-gui [input_dir] [-w work_dir] [-t script.tcl] [-v] [Qt options]
//
// The same arguments as ktplace, all optional: a design given here is read on
// start-up, a script is sourced after it, and -w is where the log and the
// pictures go. Qt's own options (-platform vnc, -style, ...) are taken first.
// Unlike ktplace, nothing runs unless asked for; the window waits for commands.

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

using namespace ktplace;

namespace {

struct GuiOptions {
    std::string inputPath;
    std::string script;
    std::string workDir = std::filesystem::current_path().string();
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
        } else if (arg == "-v" || arg == "--verbose") {
            options.verbose = true;
        } else if (!arg.empty() && arg.front() != '-' && options.inputPath.empty()) {
            options.inputPath = arg;
        } else {
            throw std::runtime_error("unknown command-line option: " + std::string(arg));
        }
    }
    std::filesystem::create_directories(options.workDir);
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
        ktlog.echo("usage: ktplace-gui [input_dir] [-w work_dir] [-t script.tcl] [-v]");
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
        engine.setGlobal("algorithm", QStringLiteral("simpl"));

        MainWindow window(engine);
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
