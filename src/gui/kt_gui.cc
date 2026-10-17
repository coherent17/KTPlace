#include "gui/kt_gui.h"

#include "kt_option.h"

#include "gui/kt_mainWindow.h"
#include "util/kt_log.h"

#include <QApplication>
#include <QTimer>
#include <filesystem>

namespace ktplace {

int runGui(const kt_option &options, int &argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KTPlace"));
    QApplication::setApplicationVersion(
        QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));
    ktlog.throwOnFatal(true);

    const bool workDirGiven = options.workDir != std::filesystem::current_path().string();
    gui::RunRequest request{options.inputPath,
                            options.inputPath.empty() && !workDirGiven ? "" : options.workDir,
                            options.tclFile};
    gui::MainWindow window(request);
    window.show();
    if (!request.design.empty()) {
        QTimer::singleShot(0, &window, [&window] {
            window.run();
        });
    }
    return QApplication::exec();
}

}  // namespace ktplace
