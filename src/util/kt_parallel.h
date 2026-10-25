#pragma once

#include <cstddef>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <vector>

namespace ktplace {

template <typename Term>
double orderedSum(std::size_t n, Term term) {
    std::vector<double> terms(n);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 1024),
                      [&](const tbb::blocked_range<std::size_t> &range) {
                          for (std::size_t i = range.begin(); i < range.end(); ++i) {
                              terms[i] = term(i);
                          }
                      });
    double total = 0.0;
    for (const double t : terms) {
        total += t;
    }
    return total;
}

}  // namespace ktplace
