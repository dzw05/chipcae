#include "test_support.hpp"

#include <algorithm>
#include <array>

using namespace resdec_test;

namespace {
void check_sampling(std::uint32_t k, std::uint32_t m, bool use_default = false) {
    Batches batches;
    auto decimator = use_default ? ResidualDecimator(collect(batches))
                                 : ResidualDecimator(collect(batches), {.max_intermediate = m});
    for (std::uint32_t i = 0; i < k + 2; ++i)
        decimator.on_iteration(
            {"sample", "R", i, static_cast<double>(i) + 0.25, flag::none, i + 10});
    decimator.on_window_end({0, 0.0});
    require(batches.size() == 1, "nonempty window must emit exactly one batch");
    const auto& batch = batches.front();
    const auto retained = std::min(k, m);
    require(batch.window == 0 && batch.time == 0.0, "window summary");
    require(batch.points.size() == 2 + retained, "endpoints plus min(k, M) samples");
    require(batch.dropped_intermediate == k - retained, "eligible points omitted");
    require(batch.points.front() ==
                ResidualBatchPoint{"sample", "R", 0, 0.25, flag::window_first, 10, 0},
            "first endpoint excluded from eligible candidates");
    require(batch.points.back() == ResidualBatchPoint{"sample",
                                                      "R",
                                                      k + 1,
                                                      static_cast<double>(k + 1) + 0.25,
                                                      flag::window_last,
                                                      k + 11,
                                                      k + 2},
            "last endpoint carries every iteration call");
    for (std::uint32_t j = 0; j < retained; ++j) {
        // Independent contract formula, with wide products for these bounded fixtures.
        const auto index = k <= m ? j : ((2ULL * j + 1) * k) / (2ULL * m);
        const auto iteration = static_cast<std::uint32_t>(index + 1);
        const ResidualBatchPoint expected{"sample",
                                          "R",
                                          iteration,
                                          static_cast<double>(iteration) + 0.25,
                                          flag::none,
                                          iteration + 10,
                                          0};
        require(batch.points[j + 1] == expected,
                "retained candidate " + std::to_string(j) + " must equal midpoint formula");
    }
    require(decimator.batches_emitted() == 1, "successful batch count");
}
} // namespace

int main() {
    Suite suite;
    for (const auto m : std::array<std::uint32_t, 4>{0, 1, 2, 16}) {
        for (const auto k : std::array<std::uint32_t, 6>{0, 1, m, m + 1, 2 * m, 1000}) {
            suite.run("k=" + std::to_string(k) + ", M=" + std::to_string(m),
                      [=] { check_sampling(k, m); });
        }
    }
    suite.run("default M is 16", [] { check_sampling(1000, 16, true); });
    return suite.result();
}
