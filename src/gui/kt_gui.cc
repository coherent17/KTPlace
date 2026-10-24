#include "gui/kt_gui.h"

#include "kt_option.h"

#include "gui/kt_mainWindow.h"
#include "util/kt_log.h"

#include <QApplication>
#include <QTimer>
#include <QtGlobal>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/utsname.h>
#include <vector>

namespace ktplace {

namespace {

bool runningUnderWsl() {
    utsname uts{};
    if (uname(&uts) != 0) {
        return false;
    }
    const std::string_view release = uts.release;
    return release.contains("microsoft") || release.contains("Microsoft");
}

bool prefersX11() {
    return runningUnderWsl() && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM") &&
           !qEnvironmentVariableIsEmpty("DISPLAY");
}

}  // namespace

int runGui(const kt_option &options, int &argc, char *argv[]) {
    std::string platformFlag = "-platform";
    std::string platformName = "xcb";
    std::vector<char *> qtArgs(argv, argv + argc);
    if (prefersX11()) {
        qtArgs.push_back(platformFlag.data());
        qtArgs.push_back(platformName.data());
    }
    int qtArgc = static_cast<int>(qtArgs.size());
    qtArgs.push_back(nullptr);
    QApplication app(qtArgc, qtArgs.data());
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
