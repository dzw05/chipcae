#include "test_support.hpp"

#include <limits>

using namespace resdec_test;

namespace {
constexpr std::uint32_t caller_unknown = std::uint32_t{1} << 29;
}

int main() {
    Suite suite;
    suite.run("repeated special merges preserve original norm and counts", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 0});
        decimator.on_iteration({"merge", "R", 3, -3.5, flag::linear_summary, 17});
        decimator.on_special(
            {SpecialKind::NotConverged,
             {"merge", "R", 3, std::numeric_limits<double>::quiet_NaN(), flag::interface, 999}});
        decimator.on_special({SpecialKind::NotConverged, {"merge", "R", 3, 99.5, flag::none, 998}});
        decimator.on_special({SpecialKind::MaxIter, {"merge", "R", 3, 98.5, caller_unknown, 997}});
        decimator.on_window_end({1, 0.25});
        expect_batches(
            batches,
            {{1,
              0.25,
              {{"merge",
                "R",
                3,
                -3.5,
                flag::window_first | flag::window_last | flag::linear_summary | flag::interface |
                    flag::not_converged | flag::max_iter | caller_unknown,
                17,
                1}},
              0}});
        require(decimator.batches_emitted() == 1, "merged singleton emitted once");
    });
    suite.run("different iteration and earlier match append in rollback order", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches));
        decimator.on_iteration({"rollback", "T", 1, 1.25, flag::none, 11});
        decimator.on_iteration({"rollback", "T", 2, 2.25, flag::none, 12});
        decimator.on_iteration({"rollback", "T", 3, 3.25, flag::none, 13});
        decimator.on_special({SpecialKind::Cutback, {"rollback", "T", 3, 93.25, flag::none, 93}});
        decimator.on_iteration({"rollback", "T", 1, 11.25, flag::none, 21});
        decimator.on_iteration({"rollback", "T", 2, 12.25, flag::none, 22});
        decimator.on_iteration({"rollback", "T", 3, 13.25, flag::none, 23});
        decimator.on_special({SpecialKind::Diverged,
                              {"rollback",
                               "T",
                               4,
                               std::numeric_limits<double>::infinity(),
                               flag::linear_summary,
                               34}});
        // Iteration 2 exists earlier, but the latest point is 4: append, never search back.
        decimator.on_special(
            {SpecialKind::NotConverged, {"rollback", "T", 2, -22.5, flag::interface, 42}});
        decimator.on_special(
            {SpecialKind::MaxIter, {"rollback", "T", 2, 99.5, caller_unknown, 99}});
        decimator.on_window_end({2, 0.5});
        expect_batches(batches,
                       {{2,
                         0.5,
                         {{"rollback", "T", 1, 1.25, flag::window_first, 11, 0},
                          {"rollback", "T", 2, 2.25, flag::none, 12, 0},
                          {"rollback", "T", 3, 3.25, flag::cutback, 13, 0},
                          {"rollback", "T", 1, 11.25, flag::none, 21, 0},
                          {"rollback", "T", 2, 12.25, flag::none, 22, 0},
                          {"rollback", "T", 3, 13.25, flag::none, 23, 0},
                          {"rollback",
                           "T",
                           4,
                           std::numeric_limits<double>::infinity(),
                           flag::diverged | flag::linear_summary,
                           34,
                           0},
                          {"rollback",
                           "T",
                           2,
                           -22.5,
                           flag::not_converged | flag::max_iter | flag::interface | caller_unknown |
                               flag::window_last,
                           42,
                           6}},
                         0}});
    });
    suite.run("all nonzero caller bits survive M zero", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 0});
        decimator.on_iteration({"flags", "R", 1, 1.5, flag::none, 11});
        decimator.on_iteration({"flags", "R", 2, -2.5, flag::linear_summary, 12});
        decimator.on_iteration({"flags", "R", 3, 3.5, flag::interface, 13});
        decimator.on_iteration({"flags", "R", 4, 4.5, caller_unknown, 14});
        decimator.on_iteration({"flags", "R", 5, 5.5, flag::none, 15});
        decimator.on_iteration({"flags", "R", 6, 6.5, flag::none, 16});
        decimator.on_window_end({0, 0.0});
        expect_batches(batches,
                       {{0,
                         0.0,
                         {{"flags", "R", 1, 1.5, flag::window_first, 11, 0},
                          {"flags", "R", 2, -2.5, flag::linear_summary, 12, 0},
                          {"flags", "R", 3, 3.5, flag::interface, 13, 0},
                          {"flags", "R", 4, 4.5, caller_unknown, 14, 0},
                          {"flags", "R", 6, 6.5, flag::window_last, 16, 6}},
                         1}});
    });
    suite.run("case sensitive and empty keys group in first seen order", [] {
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 0});
        decimator.on_iteration({"thermal", "T", 1, 1.5, flag::none, 11});
        decimator.on_special({SpecialKind::Cutback, {"", "", 7, -7.5, flag::none, 17}});
        decimator.on_iteration({"Thermal", "T", 1, 2.5, flag::none, 21});
        decimator.on_iteration({"thermal", "t", 1, 3.5, flag::none, 31});
        decimator.on_iteration({"", "T", 1, 4.5, flag::none, 41});
        decimator.on_iteration({"thermal", "", 1, 5.5, flag::none, 51});
        decimator.on_iteration({"thermal", "T", 2, 6.5, flag::none, 12});
        decimator.on_special({SpecialKind::MaxIter, {"", "", 7, 97.5, flag::interface, 97}});
        decimator.on_window_end({1, 0.5});
        const auto both = flag::window_first | flag::window_last;
        expect_batches(
            batches,
            {{1,
              0.5,
              {{"thermal", "T", 1, 1.5, flag::window_first, 11, 0},
               {"thermal", "T", 2, 6.5, flag::window_last, 12, 2},
               {"", "", 7, -7.5, both | flag::cutback | flag::max_iter | flag::interface, 17, 0},
               {"Thermal", "T", 1, 2.5, both, 21, 1},
               {"thermal", "t", 1, 3.5, both, 31, 1},
               {"", "T", 1, 4.5, both, 41, 1},
               {"thermal", "", 1, 5.5, both, 51, 1}},
              0}});
    });
    suite.run("appended special NaN payload and infinity survive unchanged", [] {
        const auto nan = std::bit_cast<double>(std::uint64_t{0x7ff8000000001234});
        const auto inf = std::numeric_limits<double>::infinity();
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 0});
        decimator.on_special(
            {SpecialKind::NotConverged, {"special", "R", 1, -1.5, flag::none, 11}});
        decimator.on_special({SpecialKind::Diverged, {"special", "R", 2, nan, caller_unknown, 12}});
        decimator.on_special(
            {SpecialKind::MaxIter, {"special", "R", 2, 99.5, flag::interface, 99}});
        decimator.on_special({SpecialKind::Diverged, {"special", "R", 3, inf, flag::none, 13}});
        decimator.on_window_end({3, 0.75});
        require(batches.size() == 1, "special-only window must emit");
        expect_batch_bits(
            batches.front(),
            {3,
             0.75,
             {{"special", "R", 1, -1.5, flag::not_converged | flag::window_first, 11, 0},
              {"special",
               "R",
               2,
               nan,
               flag::diverged | flag::max_iter | flag::interface | caller_unknown,
               12,
               0},
              {"special", "R", 3, inf, flag::diverged | flag::window_last, 13, 0}},
             0});
        require(decimator.batches_emitted() == 1, "special-only successful batch");
    });
    return suite.result();
}
