#include "legalizer/abacus/abacus_flowMgr.h"

namespace ktplace::abacus {

abacusSolution flowMgr::runLegalizer(const LegalizeParams &params) {
    return dm.run(params);
}

}  // namespace ktplace::abacus
