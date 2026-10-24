#include "kt_batch.h"
#include "kt_option.h"
#include "kt_shell.h"

#include "gui/kt_gui.h"
#include "util/kt_log.h"

#include <exception>
#include <iostream>
#include <utility>

using namespace ktplace;

int main(int argc, char *argv[]) try {
    kt_option options;
    if (!options.parse(argc, argv)) {
        return 0;
    }
    switch (options.mode) {
        case Mode::Batch:
            return runBatch(options);
        case Mode::Shell:
            return runShell(options, std::cin);
        case Mode::Gui:
            return runGui(options, argc, argv);
    }
    std::unreachable();
} catch (const std::exception &e) {
    ktlog.fatal("Error: {}", e.what());
} catch (...) {
    ktlog.fatal("Error: unknown exception");
}
