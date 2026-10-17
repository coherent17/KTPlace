#include "kt_option.h"

#include "gui/kt_mainWindow.h"
#include "util/kt_log.h"

#include <QApplication>
#include <QTimer>
#include <cstdlib>

using namespace ktplace;

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("KTPlace"));
    QApplication::setApplicationVersion(
        QString::fromUtf8(version.data(), static_cast<qsizetype>(version.size())));
    ktlog.throwOnFatal(true);

    gui::RunRequest request;
    if (argc > 1) {
        kt_option options;
        try {
            if (!options.parse(argc, argv)) {
                return EXIT_SUCCESS;
            }
        } catch (const FatalError &) {
            return EXIT_FAILURE;
        }
        request = {options.inputPath, options.workDir, options.tclFile};
    }

    gui::MainWindow window(request);
    window.show();
    if (!request.design.empty()) {
        QTimer::singleShot(0, &window, [&window] {
            window.run();
        });
    }
    return QApplication::exec();
}
