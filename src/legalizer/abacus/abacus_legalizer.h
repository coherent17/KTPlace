#pragma once

#include "legalizer/abacus/abacus_dm.h"
#include "legalizer/abacus/abacus_options.h"

#include <memory>

namespace ktplace::abacus {

class AbacusLegalizer {
public:
    explicit AbacusLegalizer(Design &design);
    ~AbacusLegalizer();

    AbacusLegalizer(const AbacusLegalizer &) = delete;
    AbacusLegalizer &operator=(const AbacusLegalizer &) = delete;

    AbacusResult legalize(const AbacusOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::abacus
