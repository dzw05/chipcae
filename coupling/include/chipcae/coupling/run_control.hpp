#pragma once

#include "chipcae/coupling/control_channel.hpp"
#include "chipcae/io/event_log.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace chipcae::coupling {
struct ControlCapabilities {
    bool set_output_interval = false;
    bool write_output_now = false;
    bool checkpoint_and_stop = false;
};

struct BoundaryDecision {
    bool stop = false;
    bool checkpoint_and_stop = false;
    bool write_output_now = false;
    std::optional<std::uint64_t> output_interval_steps;
    std::optional<double> output_interval_time;
    bool operator==(const BoundaryDecision&) const = default;
};

struct RunControlOptions {
    /// Both durations must be positive; otherwise construction throws std::invalid_argument.
    std::chrono::milliseconds pause_poll{200};
    std::chrono::milliseconds paused_heartbeat{5000};
    std::function<void(std::chrono::milliseconds)> sleep;
};

/// Collective over the same communicator as ControlChannel; callbacks must not disrupt
/// collective progress. Capabilities and durations must agree across ranks.
class RunControl {
public:
    RunControl(ControlChannel& channel,
               ControlCapabilities capabilities,
               std::function<void(io::EventPayload)> emit,
               RunControlOptions options = {});
    /// Process all commands, waiting while paused with the same completed mark.
    /// Only rank 0 emits acknowledgements and state events. Poll exceptions propagate.
    BoundaryDecision at_boundary(const WindowMark& completed);
    bool paused() const noexcept;

private:
    void emit(io::EventPayload payload) const;

    ControlChannel& channel_;
    ControlCapabilities capabilities_;
    std::function<void(io::EventPayload)> emit_;
    RunControlOptions options_;
    bool paused_ = false;
};
} // namespace chipcae::coupling
