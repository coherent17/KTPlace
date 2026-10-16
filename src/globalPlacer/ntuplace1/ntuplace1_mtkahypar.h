#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ktplace::ntuplace1 {

struct Bisection {
    std::vector<std::size_t> edgeOffsets{0};
    std::vector<std::uint64_t> pins;
    std::vector<std::int32_t> edgeWeights;
    std::vector<std::int32_t> vertexWeights;
    std::vector<std::int32_t> fixedBlock;
    std::array<std::int32_t, 2> capacity{};
};

class MtKahypar {
public:
    MtKahypar();
    ~MtKahypar();
    MtKahypar(const MtKahypar &) = delete;
    MtKahypar &operator=(const MtKahypar &) = delete;

    [[nodiscard]] std::vector<std::uint8_t> bisect(const Bisection &problem);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace::ntuplace1
