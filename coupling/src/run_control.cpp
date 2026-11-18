#include "chipcae/coupling/run_control.hpp"

#include "chipcae/core/communicator.hpp"

#include <cmath>
#include <stdexcept>
#include <thread>
#include <utility>

namespace chipcae::coupling {
RunControl::RunControl(ControlChannel& channel,
                       ControlCapabilities capabilities,
                       std::function<void(io::EventPayload)> emit,
                       RunControlOptions options)
    : channel_(channel), capabilities_(capabilities), emit_(std::move(emit)),
      options_(std::move(options)) {
    if (options_.pause_poll.count() <= 0 || options_.paused_heartbeat.count() <= 0)
        throw std::invalid_argument("run control durations must be positive");
    if (!options_.sleep)
        options_.sleep = [](auto duration) { std::this_thread::sleep_for(duration); };
}

void RunControl::emit(io::EventPayload payload) const {
    if (emit_ && Communicator::world().rank() == 0)
        emit_(std::move(payload));
}

BoundaryDecision RunControl::at_boundary(const WindowMark& completed) {
    BoundaryDecision decision;
    std::chrono::milliseconds waited{0};
    for (;;) {
        for (const auto& command : channel_.poll(completed)) {
            std::uint32_t status = control_ack::applied;
            std::string message;
            const bool was_paused = paused_;
            if (decision.stop || decision.checkpoint_and_stop) {
                status = control_ack::rejected;
                message = "run stopping";
            } else {
                switch (command.action) {
                case io::ControlAction::pause:
                    status = paused_ ? control_ack::noop : control_ack::applied;
                    paused_ = true;
                    break;
                case io::ControlAction::resume:
                    status = paused_ ? control_ack::applied : control_ack::noop;
                    paused_ = false;
                    break;
                case io::ControlAction::stop:
                    decision.stop = true;
                    paused_ = false;
                    break;
                case io::ControlAction::checkpoint_and_stop:
                    if (!capabilities_.checkpoint_and_stop) {
                        status = control_ack::rejected;
                        message = "unsupported";
                    } else {
                        decision.checkpoint_and_stop = true;
                        paused_ = false;
                    }
                    break;
                case io::ControlAction::write_output_now:
                    if (!capabilities_.write_output_now) {
                        status = control_ack::rejected;
                        message = "unsupported";
                    } else {
                        decision.write_output_now = true;
                    }
                    break;
                case io::ControlAction::set_output_interval:
                    if (!capabilities_.set_output_interval) {
                        status = control_ack::rejected;
                        message = "unsupported";
                    } else if (command.has_interval_steps == command.has_interval_time ||
                               (command.has_interval_steps && command.output_interval_steps == 0) ||
                               (command.has_interval_time &&
                                (!std::isfinite(command.output_interval_time) ||
                                 command.output_interval_time <= 0))) {
                        status = control_ack::rejected;
                        message = "invalid interval";
                    } else if (command.has_interval_steps) {
                        decision.output_interval_steps = command.output_interval_steps;
                        decision.output_interval_time.reset();
                    } else {
                        decision.output_interval_time = command.output_interval_time;
                        decision.output_interval_steps.reset();
                    }
                    break;
                }
            }
            emit(make_control_ack(command, status, completed, std::move(message)));
            if (!was_paused && paused_) {
                waited = std::chrono::milliseconds{0};
                emit(io::State{io::State::Value::paused, completed.window, completed.time});
            } else if (was_paused && !paused_ && !decision.stop && !decision.checkpoint_and_stop) {
                emit(io::State{io::State::Value::running, completed.window, completed.time});
            }
        }
        if (!paused_)
            return decision;

        options_.sleep(options_.pause_poll);
        // Consume the remaining duration in pieces to retain the heartbeat remainder
        // without overflowing the duration representation when adding pause_poll.
        auto remaining = options_.pause_poll;
        while (remaining >= options_.paused_heartbeat - waited) {
            remaining -= options_.paused_heartbeat - waited;
            waited = std::chrono::milliseconds{0};
            emit(io::State{io::State::Value::paused, completed.window, completed.time});
        }
        waited += remaining;
    }
}

bool RunControl::paused() const noexcept {
    return paused_;
}
} // namespace chipcae::coupling
