#pragma once

#include "legalizer/multirow/multirow_dm.h"
#include "legalizer/multirow/multirow_options.h"

#include <memory>

namespace ktplace::multirow {

class MultiRowLegalizer {
public:
    explicit MultiRowLegalizer(Design &design);
    ~MultiRowLegalizer();

    MultiRowLegalizer(const MultiRowLegalizer &) = delete;
    MultiRowLegalizer &operator=(const MultiRowLegalizer &) = delete;
    MultiRowLegalizer(MultiRowLegalizer &&) noexcept;
    MultiRowLegalizer &operator=(MultiRowLegalizer &&) noexcept;

    void legalize(const MultiRowOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::multirow
