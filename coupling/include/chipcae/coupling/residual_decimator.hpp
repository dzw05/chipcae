#pragma once

#include "chipcae/io/event_log.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace chipcae::coupling {
namespace residual_flag {
inline constexpr std::uint32_t none = 0, window_first = 1, window_last = 2, not_converged = 4,
                               diverged = 8, cutback = 16, max_iter = 32, linear_summary = 64,
                               interface = 128;
}

struct ResidualRecord {
    std::string solver, field;
    std::uint32_t iteration{};
    double norm{};
    std::uint32_t flags{};
    std::uint32_t linear_iterations{};
};

enum class SpecialKind : std::uint32_t {
    NotConverged = 4,
    Diverged = 8,
    Cutback = 16,
    MaxIter = 32
};
struct SpecialPoint {
    SpecialKind kind;
    ResidualRecord record;
};
struct WindowSummary {
    std::uint64_t window{};
    double time{};
};
struct ResidualDecimatorOptions {
    std::uint32_t max_intermediate = 16;
};
using ResidualBatchSink = std::function<void(const io::ResidualBatch&)>;

/// Single-threaded; rank 0 only; no MPI, no I/O.
class ResidualDecimator {
public:
    explicit ResidualDecimator(ResidualBatchSink sink, ResidualDecimatorOptions options = {});
    void on_iteration(const ResidualRecord& record);
    void on_special(SpecialPoint point);
    /// Emit at most one batch for this window through the sink, then clear state.
    void on_window_end(const WindowSummary& summary);
    std::uint64_t batches_emitted() const noexcept;

private:
    struct Sequence {
        std::vector<io::ResidualBatchPoint> points;
        std::uint32_t iteration_calls{};
    };
    Sequence& sequence_for(const ResidualRecord& record);

    ResidualBatchSink sink_;
    ResidualDecimatorOptions options_;
    std::vector<Sequence> sequences_;
    std::map<std::pair<std::string, std::string>, std::size_t> sequence_lookup_;
    std::optional<std::uint64_t> previous_window_;
    std::uint64_t batches_emitted_{};
};
} // namespace chipcae::coupling
