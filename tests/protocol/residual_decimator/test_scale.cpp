#include "test_support.hpp"

using namespace resdec_test;

int main() {
    Suite suite;
    suite.run("100000 iteration calls retain exact midpoint samples", [] {
        constexpr std::uint32_t total = 100000;
        constexpr std::uint32_t m = 16;
        constexpr std::uint32_t eligible = total - 2;
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = m});
        const auto started = std::chrono::steady_clock::now();
        for (std::uint32_t i = 1; i <= total; ++i)
            decimator.on_iteration(
                {"scale", "R", i, static_cast<double>(i) + 0.5, flag::none, i + 7});
        decimator.on_window_end({42, 2.5});
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "Measured elapsed: " << std::chrono::duration<double>(elapsed).count()
                  << " s\n";
        require(batches.size() == 1, "100000 iterations must emit one batch");
        const auto& batch = batches.front();
        require(batch.window == 42 && batch.time == 2.5, "scale window summary");
        require(batch.points.size() == 18, "16 sampled points plus two endpoints");
        require(batch.dropped_intermediate == 99982, "exact omitted count");
        require(batch.points.front() ==
                    ResidualBatchPoint{"scale", "R", 1, 1.5, flag::window_first, 8, 0},
                "first point preserved");
        require(batch.points.back() ==
                    ResidualBatchPoint{
                        "scale", "R", total, 100000.5, flag::window_last, total + 7, total},
                "last point counts all 100000 calls");
        for (std::uint32_t j = 0; j < m; ++j) {
            const auto index = ((2ULL * j + 1) * eligible) / (2ULL * m);
            const auto iteration = static_cast<std::uint32_t>(index + 2);
            require(batch.points[j + 1] == ResidualBatchPoint{"scale",
                                                              "R",
                                                              iteration,
                                                              static_cast<double>(iteration) + 0.5,
                                                              flag::none,
                                                              iteration + 7,
                                                              0},
                    "exact sampled index " + std::to_string(j));
        }
        require(decimator.batches_emitted() == 1, "one successful scale batch");
    });
    return suite.result();
}
