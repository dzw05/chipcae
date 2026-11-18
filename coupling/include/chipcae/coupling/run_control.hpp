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
    std::chrono::milliseconds pause_poll{200};
    std::chrono::milliseconds paused_heartbeat{5000};
    std::function<void(std::chrono::milliseconds)> sleep;
};

// TEST-FIRST scaffold only: dispatch and pause behavior are deliberately unimplemented.
class RunControl {
public:
    RunControl(ControlChannel& channel,
               ControlCapabilities capabilities,
               std::function<void(io::EventPayload)> emit,
               RunControlOptions options = {});
    BoundaryDecision at_boundary(const WindowMark& completed);
    bool paused() const noexcept;
};
} // namespace chipcae::coupling
