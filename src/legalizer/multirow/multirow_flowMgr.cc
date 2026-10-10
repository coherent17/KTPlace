#include "legalizer/multirow/multirow_flowMgr.h"

namespace ktplace::multirow {

multiRowSolution flowMgr::runLegalizer(const MultiRowLegalizeParams &params) {
    return dm.run(params);
}

}  // namespace ktplace::multirow
