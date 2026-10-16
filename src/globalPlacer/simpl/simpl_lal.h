#pragma once

#include "globalPlacer/simpl/simpl_dm.h"
#include "globalPlacer/simpl/simpl_grid.h"
#include "globalPlacer/simpl/simpl_options.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace ktplace::simpl {

struct Block;
struct Stripe;

struct LookAheadLegalizer {
    const BinGrid &grid;
    const FreeSpace &freeSpace;
    const std::vector<Box> &obstacles;
    const std::vector<double> &width;
    const std::vector<double> &height;
    const std::vector<double> &area;
    const SimplOptions &opts;
    double densityLimit;
    DieBox die;

    void legalize(std::vector<double> &x, std::vector<double> &y) const;

private:
    [[nodiscard]] std::size_t numMovable() const {
        return width.size();
    }
    [[nodiscard]] double centre(const std::vector<double> &pos, std::uint32_t i,
                                bool vertical) const;
    [[nodiscard]] double density(const std::vector<double> &occ, const BinRect &r) const;
    [[nodiscard]] std::vector<BinRect> overfilledClusters(const std::vector<double> &occ) const;
    [[nodiscard]] BinRect grown(BinRect r, const std::vector<double> &occ) const;
    void legalizeBlock(Block &block, std::vector<double> &x, std::vector<double> &y) const;
    [[nodiscard]] std::optional<std::pair<Block, Block>> split(Block &region,
                                                               std::vector<double> &x,
                                                               std::vector<double> &y) const;
    void nonlinearScale(const std::vector<std::uint32_t> &cells, const Box &box, bool vertical,
                        double cutline, bool belowCut, std::vector<double> &pos,
                        double regionRoom) const;
    [[nodiscard]] std::vector<Stripe> stripes(const Box &box, bool vertical, double cellArea,
                                              double regionRoom) const;
    void clampToDie(std::vector<double> &x, std::vector<double> &y) const;
};

}  // namespace ktplace::simpl
