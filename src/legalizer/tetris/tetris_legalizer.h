#pragma once

#include "legalizer/tetris/tetris_dm.h"
#include "legalizer/tetris/tetris_options.h"

#include <memory>

namespace ktplace::tetris {

class TetrisLegalizer {
public:
    explicit TetrisLegalizer(Design &design);
    ~TetrisLegalizer();

    TetrisLegalizer(const TetrisLegalizer &) = delete;
    TetrisLegalizer &operator=(const TetrisLegalizer &) = delete;

    void legalize(const TetrisOptions &options = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::tetris
