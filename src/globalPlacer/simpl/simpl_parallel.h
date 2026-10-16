#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_sort.h>
#include <vector>

namespace ktplace::simpl {

inline constexpr std::size_t grain = 4096;

template <typename Body>
void parallelFor(std::size_t n, Body body) {
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, grain),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t i = range.begin(); i < range.end(); ++i) {
                              body(i);
                          }
                      });
}

template <typename Key>
void sortCells(std::vector<std::uint32_t> &cells, Key key) {
    tbb::parallel_sort(cells.begin(), cells.end(), [&](std::uint32_t a, std::uint32_t b) {
        const double ka = key(a);
        const double kb = key(b);
        return ka < kb || (ka == kb && a < b);
    });
}

template <typename Body>
double deterministicSum(std::size_t n, Body body) {
    const std::size_t chunks = (n + grain - 1) / grain;
    std::vector<double> partial(chunks, 0.0);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, chunks, 1),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t c = range.begin(); c < range.end(); ++c) {
                              const std::size_t end = std::min(n, (c + 1) * grain);
                              double sum = 0.0;
                              for (std::size_t i = c * grain; i < end; ++i) {
                                  sum += body(i);
                              }
                              partial[c] = sum;
                          }
                      });
    return std::accumulate(partial.begin(), partial.end(), 0.0);
}

inline double dot(const std::vector<double> &a, const std::vector<double> &b) {
    return deterministicSum(a.size(), [&](std::size_t i) {
        return a[i] * b[i];
    });
}

}  // namespace ktplace::simpl
