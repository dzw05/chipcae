#include "test_support.hpp"

#include <exception>
#include <limits>

using namespace resdec_test;

int main() {
    Suite suite;
    suite.run("empty sink is rejected", [] {
        expect_invalid_argument([] { ResidualDecimator decimator(ResidualBatchSink{}); },
                                "empty sink constructor");
    });
    suite.run("first window zero is accepted", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches));
        decimator.on_iteration({"zero", "R", 1, 0.5, flag::none, 7});
        decimator.on_window_end({0, 0.0});
        expect_batches(
            batches,
            {{0, 0.0, {{"zero", "R", 1, 0.5, flag::window_first | flag::window_last, 7, 1}}, 0}});
        require(decimator.batches_emitted() == 1, "zero window counted");
    });
    suite.run("empty windows advance ordering without emission", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches));
        decimator.on_window_end({0, 0.0});
        decimator.on_window_end({3, 1.0});
        require(batches.empty(), "empty windows must not call sink");
        require(decimator.batches_emitted() == 0, "empty windows do not count as emissions");
        expect_invalid_argument([&] { decimator.on_window_end({3, 2.0}); }, "equal empty window");
        expect_invalid_argument([&] { decimator.on_window_end({2, 2.0}); },
                                "decreasing empty window");
        decimator.on_iteration({"after-empty", "R", 9, 9.5, flag::none, 8});
        decimator.on_window_end({4, 0.25});
        expect_batches(
            batches,
            {{4,
              0.25,
              {{"after-empty", "R", 9, 9.5, flag::window_first | flag::window_last, 8, 1}},
              0}});
        require(decimator.batches_emitted() == 1, "only nonempty window counted");
    });
    suite.run("invalid summaries preserve pending records and window state", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches));
        decimator.on_window_end({5, 10.0});
        decimator.on_iteration({"pending", "T", 2, -2.25, flag::interface, 12});
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const auto inf = std::numeric_limits<double>::infinity();
        for (const auto summary : std::vector<WindowSummary>{
                 {5, 1.0}, {4, 1.0}, {5, nan}, {6, nan}, {6, inf}, {6, -inf}}) {
            expect_invalid_argument([&] { decimator.on_window_end(summary); }, "invalid summary");
            require(batches.empty(), "invalid summary must not emit or discard pending data");
            require(decimator.batches_emitted() == 0, "invalid summary must not count");
        }
        decimator.on_iteration({"pending", "T", 3, 3.25, flag::none, 13});
        decimator.on_window_end({6, 0.5});
        expect_batches(batches,
                       {{6,
                         0.5,
                         {{"pending", "T", 2, -2.25, flag::interface | flag::window_first, 12, 0},
                          {"pending", "T", 3, 3.25, flag::window_last, 13, 2}},
                         0}});
        require(decimator.batches_emitted() == 1, "valid retry counted once");
    });
    suite.run("invalid first time does not consume the first window", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches));
        decimator.on_iteration({"first", "T", 1, 1.25, flag::none, 11});
        for (const auto time : {std::numeric_limits<double>::quiet_NaN(),
                                std::numeric_limits<double>::infinity(),
                                -std::numeric_limits<double>::infinity()})
            expect_invalid_argument([&] { decimator.on_window_end({0, time}); }, "first time");
        decimator.on_window_end({0, -0.25});
        expect_batches(batches,
                       {{0,
                         -0.25,
                         {{"first", "T", 1, 1.25, flag::window_first | flag::window_last, 11, 1}},
                         0}});
        require(decimator.batches_emitted() == 1, "valid first window counted");
    });
    suite.run("sink exception propagates unchanged and consumes failed window", [] {
        struct SinkFailure : std::runtime_error {
            using std::runtime_error::runtime_error;
        };
        const auto failure = std::make_exception_ptr(SinkFailure("distinct residual sink failure"));
        bool fail = true;
        Batches attempts;
        ResidualDecimator decimator([&](const ResidualBatch& batch) {
            attempts.push_back(batch);
            if (fail)
                std::rethrow_exception(failure);
        });
        decimator.on_iteration({"failed", "T", 4, 4.25, flag::none, 14});
        bool caught = false;
        try {
            decimator.on_window_end({4, 1.0});
        } catch (...) {
            caught = true;
            require(std::current_exception() == failure, "sink exception object must propagate");
        }
        require(caught, "nonempty window must invoke throwing sink");
        require(decimator.batches_emitted() == 0, "throwing sink is not a successful batch");
        require(attempts.size() == 1, "sink attempted exactly once");
        expect_invalid_argument([&] { decimator.on_window_end({4, 1.0}); },
                                "failed window consumed");
        require(attempts.size() == 1, "failed batch must not be retried");

        fail = false;
        decimator.on_iteration({"clean", "U", 1, 0.25, flag::none, 21});
        decimator.on_window_end({5, 1.25});
        expect_batches(
            attempts,
            {{4, 1.0, {{"failed", "T", 4, 4.25, flag::window_first | flag::window_last, 14, 1}}, 0},
             {5,
              1.25,
              {{"clean", "U", 1, 0.25, flag::window_first | flag::window_last, 21, 1}},
              0}});
        require(decimator.batches_emitted() == 1, "only clean later window counted");
        decimator.on_window_end({6, 1.5});
        require(attempts.size() == 2, "failed window data must remain cleared");
        require(decimator.batches_emitted() == 1, "empty window does not increase counter");
    });
    return suite.result();
}
