#include "globalPlacer/ntuplace1/ntuplace1_mtkahypar.h"

#include "util/kt_log.h"

#include <algorithm>
#include <mtkahypar.h>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace ktplace::ntuplace1 {
namespace {

constexpr std::size_t seed = 0;

void check(mt_kahypar_error_t &error, std::string_view what) {
    if (error.status != SUCCESS) {
        const std::string message = error.msg != nullptr ? error.msg : "unknown error";
        mt_kahypar_free_error_content(&error);
        ktlog.fatal("Mt-KaHyPar {}: {}", what, message);
    }
}

}  // namespace

class MtKahypar::Impl {
public:
    Impl() {
        static std::once_flag initialized;
        std::call_once(initialized, [] {
            mt_kahypar_initialize(std::max(1U, std::thread::hardware_concurrency()), false);
        });
        context = mt_kahypar_context_from_preset(DETERMINISTIC);
        mt_kahypar_error_t error{};
        mt_kahypar_set_context_parameter(context, VERBOSE, "0", &error);
        check(error, "verbose");
    }
    ~Impl() {
        mt_kahypar_free_context(context);
    }
    Impl(const Impl &) = delete;
    Impl &operator=(const Impl &) = delete;

    mt_kahypar_context_t *context;
};

MtKahypar::MtKahypar() : pImpl(std::make_unique<Impl>()) {}

MtKahypar::~MtKahypar() = default;

std::vector<std::uint8_t> MtKahypar::bisect(const Bisection &problem) {
    const std::size_t n = problem.vertexWeights.size();
    std::int64_t total = 0;
    for (const std::int32_t w : problem.vertexWeights) {
        total += w;
    }
    std::array<std::int32_t, 2> capacity = problem.capacity;
    const std::int64_t shortfall = total - capacity[0] - capacity[1];
    if (shortfall > 0) {
        capacity[0] += static_cast<std::int32_t>((shortfall + 1) / 2);
        capacity[1] += static_cast<std::int32_t>((shortfall + 1) / 2);
    }
    const double epsilon =
        static_cast<double>(std::max(capacity[0], capacity[1])) / (0.5 * total) - 1.0;

    mt_kahypar_set_partitioning_parameters(pImpl->context, 2, std::max(epsilon, 0.0), CUT);
    mt_kahypar_set_individual_target_block_weights(pImpl->context, 2, capacity.data());
    mt_kahypar_set_seed(seed);

    mt_kahypar_error_t error{};
    mt_kahypar_hypergraph_t graph = mt_kahypar_create_hypergraph(
        pImpl->context, n, problem.edgeWeights.size(), problem.edgeOffsets.data(),
        problem.pins.data(), problem.edgeWeights.data(), problem.vertexWeights.data(), &error);
    check(error, "hypergraph");
    mt_kahypar_add_fixed_vertices(graph, problem.fixedBlock.data(), 2, &error);
    check(error, "fixed vertices");
    mt_kahypar_partitioned_hypergraph_t partitioned =
        mt_kahypar_partition(graph, pImpl->context, &error);
    check(error, "partition");

    std::vector<mt_kahypar_partition_id_t> block(n);
    mt_kahypar_get_partition(partitioned, block.data());
    mt_kahypar_free_partitioned_hypergraph(partitioned);
    mt_kahypar_free_hypergraph(graph);

    std::vector<std::uint8_t> side(n);
    std::ranges::transform(block, side.begin(), [](mt_kahypar_partition_id_t b) {
        return static_cast<std::uint8_t>(b);
    });
    return side;
}

}  // namespace ktplace::ntuplace1
