#include "test_support.hpp"

#include <variant>

using namespace resdec_test;

int main() {
    Suite suite;
    suite.run("three emitted batches roundtrip through event log", [] {
        TempDirectory directory;
        Batches batches;
        ResidualDecimator decimator(collect(batches), {.max_intermediate = 1});
        for (std::uint32_t i = 1; i <= 5; ++i)
            decimator.on_iteration(
                {"thermal", "T", i, static_cast<double>(i) + 0.5, flag::none, i});
        decimator.on_window_end({0, 0.0});

        decimator.on_iteration({"thermal", "T", 1, 11.5, flag::none, 11});
        decimator.on_iteration({"structure", "U", 1, 21.5, flag::interface, 21});
        decimator.on_iteration({"thermal", "T", 2, 12.5, flag::none, 12});
        decimator.on_special({SpecialKind::Cutback, {"thermal", "T", 2, 92.5, flag::none, 92}});
        decimator.on_iteration({"thermal", "T", 1, 13.5, flag::none, 13});
        decimator.on_window_end({1, 0.25});

        decimator.on_special({SpecialKind::MaxIter, {"", "", 4, -4.5, flag::linear_summary, 14}});
        decimator.on_window_end({7, 1.5});
        const auto both = flag::window_first | flag::window_last;
        expect_batches(batches,
                       {{0,
                         0.0,
                         {{"thermal", "T", 1, 1.5, flag::window_first, 1, 0},
                          {"thermal", "T", 3, 3.5, flag::none, 3, 0},
                          {"thermal", "T", 5, 5.5, flag::window_last, 5, 5}},
                         2},
                        {1,
                         0.25,
                         {{"thermal", "T", 1, 11.5, flag::window_first, 11, 0},
                          {"thermal", "T", 2, 12.5, flag::cutback, 12, 0},
                          {"thermal", "T", 1, 13.5, flag::window_last, 13, 3},
                          {"structure", "U", 1, 21.5, both | flag::interface, 21, 1}},
                         0},
                        {7,
                         1.5,
                         {{"", "", 4, -4.5, both | flag::max_iter | flag::linear_summary, 14, 0}},
                         0}});
        require(decimator.batches_emitted() == 3, "three successful emissions");

        chipcae::io::EventLogWriter writer(directory.file());
        for (std::size_t i = 0; i < batches.size(); ++i)
            writer.append(chipcae::io::Event{
                static_cast<std::uint64_t>(i), 1000 + static_cast<std::int64_t>(i), batches[i]});
        writer.close();
        const auto events = chipcae::io::EventLogReader(directory.file()).read_from(0);
        require(events.size() == 3, "read_from(0) must decode exactly three events");
        for (std::size_t i = 0; i < events.size(); ++i) {
            require(events[i].seq == i, "event sequence");
            require(events[i].wall_time_unix_ns == 1000 + static_cast<std::int64_t>(i),
                    "event wall time");
            const auto* decoded = std::get_if<ResidualBatch>(&events[i].payload);
            require(decoded != nullptr, "decoded payload must be ResidualBatch");
            require(*decoded == batches[i], "decoded batch must equal captured batch in full");
        }
    });
    return suite.result();
}
