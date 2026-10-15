#include "legalizer/abacus/abacus_flowMgr.h"

namespace ktplace::abacus {

abacusSolution flowMgr::runLegalizer() {
    return dm.run();
}

}  // namespace ktplace::abacus
