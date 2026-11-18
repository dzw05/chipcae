#include "test_support.hpp"

#include <limits>

using namespace resdec_test;

int main() {
    Suite suite;
    suite.run("T-RES-01 interleaved rollback and all special kinds", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 2});
        require(decimator.batches_emitted() == 0, "initial successful count");
        const auto inf = std::numeric_limits<double>::infinity();
        decimator.on_iteration({"thermal", "T", 1, 101.0, flag::none, 11});
        decimator.on_iteration({"structure", "U", 1, 301.0, flag::none, 41});
        decimator.on_iteration({"thermal", "T", 2, 102.0, flag::none, 12});
        decimator.on_iteration({"structure", "U", 2, 302.0, flag::none, 42});
        decimator.on_iteration({"thermal", "T", 3, 103.0, flag::none, 13});
        decimator.on_special(
            {SpecialKind::NotConverged, {"thermal", "T", 3, 903.0, flag::none, 93}});
        decimator.on_iteration({"structure", "U", 3, 303.0, flag::none, 43});
        decimator.on_iteration({"thermal", "T", 4, 104.0, flag::none, 14});
        decimator.on_iteration({"structure", "U", 4, 304.0, flag::none, 44});
        decimator.on_iteration({"thermal", "T", 5, 105.0, flag::none, 15});
        decimator.on_special({SpecialKind::Cutback, {"thermal", "T", 5, 905.0, flag::none, 95}});
        decimator.on_iteration({"structure", "U", 5, 305.0, flag::none, 45});
        decimator.on_iteration({"thermal", "T", 1, 201.0, flag::none, 21});
        decimator.on_iteration({"structure", "U", 6, 306.0, flag::none, 46});
        decimator.on_iteration({"thermal", "T", 2, 202.0, flag::none, 22});
        decimator.on_iteration({"thermal", "T", 3, 203.0, flag::none, 23});
        decimator.on_special({SpecialKind::Diverged, {"thermal", "T", 4, inf, flag::none, 34}});
        decimator.on_special({SpecialKind::MaxIter, {"thermal", "T", 4, 904.0, flag::none, 94}});
        require(batches.empty(), "ingestion must not emit before window end");
        require(decimator.batches_emitted() == 0, "ingestion successful count");
        decimator.on_window_end({7, 0.5});

        // Thermal eligible positions [original 2, original 4, reset 1, reset 2, reset 3]
        // retain positions [1, 3]. Structure's four eligible points retain positions [1, 3].
        const ResidualBatch first{
            7,
            0.5,
            {{"thermal", "T", 1, 101.0, flag::window_first, 11, 0},
             {"thermal", "T", 3, 103.0, flag::not_converged, 13, 0},
             {"thermal", "T", 4, 104.0, flag::none, 14, 0},
             {"thermal", "T", 5, 105.0, flag::cutback, 15, 0},
             {"thermal", "T", 2, 202.0, flag::none, 22, 0},
             {"thermal", "T", 4, inf, flag::diverged | flag::max_iter | flag::window_last, 34, 8},
             {"structure", "U", 1, 301.0, flag::window_first, 41, 0},
             {"structure", "U", 3, 303.0, flag::none, 43, 0},
             {"structure", "U", 5, 305.0, flag::none, 45, 0},
             {"structure", "U", 6, 306.0, flag::window_last, 46, 6}},
            5};
        expect_batches(batches, {first});
        require(batches[0].points.size() == 10, "ten retained points across two sequences");
        require(decimator.batches_emitted() == 1, "first successful batch");

        decimator.on_iteration({"structure", "U", 9, 29.0, flag::none, 19});
        decimator.on_iteration({"thermal", "T", 9, 19.0, flag::none, 9});
        decimator.on_window_end({8, 0.75});
        const ResidualBatch second{
            8,
            0.75,
            {{"structure", "U", 9, 29.0, flag::window_first | flag::window_last, 19, 1},
             {"thermal", "T", 9, 19.0, flag::window_first | flag::window_last, 9, 1}},
            0};
        expect_batches(batches, {first, second});
        require(decimator.batches_emitted() == 2, "two successful batches");
    });
    return suite.result();
}
