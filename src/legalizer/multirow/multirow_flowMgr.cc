#include "legalizer/multirow/multirow_flowMgr.h"

namespace ktplace::multirow {

multiRowSolution flowMgr::runLegalizer() {
    return dm.run();
}

}  // namespace ktplace::multirow
