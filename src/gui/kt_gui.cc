// @file kt_gui.cc
// ktplace-gui: the KTPlace main window.
//
//   ktplace-gui [input_dir] [ktplace options] [-b bench_root] [Qt options]
//
// The options are ktplace's (kt_option), so `ktplace --gui` passes its own
// command line on unchanged; all are optional here. The --tcl configuration is
// loaded first, then the design is read and the --script script sourced; -w is
// where the log, the pictures and the run history (runs.json) go. -b is where
// the Runs panel finds designs (default ./benchmark, when there is one). Qt's
// own options (-platform, -style, ...) are taken first. Unlike ktplace, nothing
// else runs: the window waits for commands.

#include "kt_flowMgr.h"
#include "kt_option.h"

#include "datamodel/kt_optionDb.h"
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

using namespace ktplace;

int main(int argc, char *argv[]) {
    // QApplication removes the options it recognises from argv.
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KTPlace"));
    QApplication::setApplicationVersion(
        QString::fromUtf8(VERSION.data(), static_cast<qsizetype>(VERSION.size())));

    // ktplace's own options: `ktplace --gui` passes them on unchanged.
    kt_option options;
    try {
        if (!options.parse(argc, argv, /*inputOptional=*/true)) {
            return EXIT_SUCCESS;  // --help or --version
        }
        // The configuration (--tcl) before anything reads an option, as for
        // ktplace: it may choose the placer that ktplace(algorithm) names.
        FlowMgr::configure(options);
    } catch (const FatalError &) {
        return EXIT_FAILURE;  // already reported
    } catch (const std::exception &e) {
        ktlog.warning("{}", e.what());
        return EXIT_FAILURE;
    }
    if (options.benchRoot.empty() && std::filesystem::is_directory("benchmark")) {
        options.benchRoot = std::filesystem::absolute("benchmark").string();
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
        engine.setGlobal("algorithm",
                         QString::fromStdString(OptionDb::instance()
                                                    .getComponentOption(Component::Flow)
                                                    .get<std::string>("algorithm")
                                                    .value_or("simpl")));

        MainWindow window(engine);
        window.setHistoryFile(QString::fromStdString((work / "runs.json").string()));
        window.setBenchmarkRoot(QString::fromStdString(options.benchRoot));
        window.show();

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
