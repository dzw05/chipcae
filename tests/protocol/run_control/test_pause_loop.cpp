#include "test_support.hpp"

using namespace runctl_test;

int main() {
    Suite suite;
    suite.run("twelve polls yield two accumulated heartbeat states", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControl* observed = nullptr;
        const WindowMark mark{17, 2.25};
        RunControlOptions options;
        options.pause_poll = 200ms;
        options.paused_heartbeat = 1000ms;
        options.sleep = [&](auto duration) {
            require(duration == 200ms, "every sleep uses pause_poll");
            require(observed->paused(), "paused() must be true inside sleep");
            require(++sleeps <= 12, "failed to resume after twelve sleeps");
            const auto heartbeats = static_cast<std::size_t>((sleeps - 1) / 5);
            require(events.size() == 2 + heartbeats,
                    "heartbeat must occur at cumulative threshold");
            if (sleeps == 12)
                append(dir.file(), 2, "RESUME");
        };
        RunControl control(channel, {}, collect(events), options);
        observed = &control;
        require(!control.paused(), "not paused before first boundary");
        require(control.at_boundary(mark) == BoundaryDecision{}, "pause-loop default decision");
        require(sleeps == 12, "expected exactly 12 sleeps, got " + std::to_string(sleeps));
        require(!control.paused(), "not paused after resume");
        expect_events(events,
                      {ack(1, 1, mark),
                       state(State::Value::paused, mark),
                       state(State::Value::paused, mark),
                       state(State::Value::paused, mark),
                       ack(2, 1, mark),
                       state(State::Value::running, mark)});
    });
    suite.run("heartbeat remainder survives and heartbeat precedes threshold resume", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControlOptions options;
        options.pause_poll = 300ms;
        options.paused_heartbeat = 1000ms;
        options.sleep = [&](auto duration) {
            require(duration == 300ms, "nondivisible pause_poll");
            require(++sleeps <= 7, "nondivisible heartbeat resume");
            require(events.size() == (sleeps <= 4 ? 2U : 3U), "early/late heartbeat");
            if (sleeps == 7)
                append(dir.file(), 2, "RESUME");
        };
        RunControl control(channel, {}, collect(events), options);
        const WindowMark mark{18, 3.0};
        require(control.at_boundary(mark) == BoundaryDecision{}, "remainder decision");
        require(sleeps == 7, "seven sleeps for 2100ms");
        expect_events(events,
                      {ack(1, 1, mark),
                       state(State::Value::paused, mark),
                       state(State::Value::paused, mark),
                       state(State::Value::paused, mark),
                       ack(2, 1, mark),
                       state(State::Value::running, mark)});
    });
    suite.run("paused output accumulates and last valid interval wins", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControl* observed = nullptr;
        RunControlOptions options;
        options.sleep = [&](auto duration) {
            require(duration == 200ms && observed->paused(), "output command must remain paused");
            require(++sleeps <= 3, "paused output exit");
            if (sleeps == 1) {
                append(dir.file(), 2, "WRITE_OUTPUT_NOW");
                append(dir.file(), 3, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":10");
            } else if (sleeps == 2) {
                append(dir.file(), 4, "SET_OUTPUT_INTERVAL", ",\"output_interval_time\":0.5");
            } else {
                append(dir.file(), 5, "RESUME");
            }
        };
        RunControl control(channel, {true, true, false}, collect(events), options);
        observed = &control;
        BoundaryDecision expected;
        expected.write_output_now = true;
        expected.output_interval_time = 0.5;
        const WindowMark mark{19, 4.25};
        require(control.at_boundary(mark) == expected, "accumulated output decision");
        require(sleeps == 3 && !control.paused(), "output must not exit pause early");
        expect_events(events,
                      {ack(1, 1, mark),
                       state(State::Value::paused, mark),
                       ack(2, 1, mark),
                       ack(3, 1, mark),
                       ack(4, 1, mark),
                       ack(5, 1, mark),
                       state(State::Value::running, mark)});
    });
    suite.run("new pause episode resets heartbeat accumulation", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControlOptions options;
        options.pause_poll = 600ms;
        options.paused_heartbeat = 1000ms;
        options.sleep = [&](auto duration) {
            require(duration == 600ms, "episode pause_poll");
            require(++sleeps <= 2, "episode resume");
            append(dir.file(), sleeps == 1 ? 2 : 4, "RESUME");
        };
        RunControl control(channel, {}, collect(events), options);
        const WindowMark first{20, 5.0}, second{21, 5.5};
        require(control.at_boundary(first) == BoundaryDecision{}, "first episode");
        append(dir.file(), 3, "PAUSE");
        require(control.at_boundary(second) == BoundaryDecision{}, "second episode");
        require(sleeps == 2, "one sleep in each episode");
        expect_events(events,
                      {ack(1, 1, first),
                       state(State::Value::paused, first),
                       ack(2, 1, first),
                       state(State::Value::running, first),
                       ack(3, 1, second),
                       state(State::Value::paused, second),
                       ack(4, 1, second),
                       state(State::Value::running, second)});
    });
    return suite.result();
}
