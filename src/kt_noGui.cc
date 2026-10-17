#include "kt_option.h"

#include "gui/kt_gui.h"
#include "util/kt_log.h"

namespace ktplace {

int runGui(const kt_option &, int &, char *[]) {
    ktlog.fatal(
        "this ktplace was built without the GUI; rebuild it with 'make GUI=1' (needs Qt 6)");
}

}  // namespace ktplace
