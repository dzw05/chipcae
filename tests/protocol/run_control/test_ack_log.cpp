#include "test_support.hpp"

using namespace runctl_test;

int main() {
    Suite suite;
    suite.run("pause-loop events round trip through event log in exact order", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        int sleeps = 0;
        RunControlOptions options;
        options.pause_poll = 200ms;
        options.paused_heartbeat = 1000ms;
        options.sleep = [&](auto duration) {
            require(duration == 200ms, "logged pause_poll");
            require(++sleeps <= 12, "logged resume must end pause");
            if (sleeps == 12)
                append(dir.file(), 2, "RESUME");
        };
        const WindowMark mark{51, 15.25};
        {
            chipcae::io::EventLogWriter writer(dir.file("events.bin"));
            std::uint64_t seq = 0;
            RunControl control(
                channel,
                {},
                [&](EventPayload payload) {
                    writer.append({++seq, 123456789, std::move(payload)});
                },
                options);
            require(control.at_boundary(mark) == BoundaryDecision{}, "logged decision");
            writer.close();
        }
        const Events expected{ack(1, 1, mark),
                              state(State::Value::paused, mark),
                              state(State::Value::paused, mark),
                              state(State::Value::paused, mark),
                              ack(2, 1, mark),
                              state(State::Value::running, mark)};
        chipcae::io::EventLogReader reader(dir.file("events.bin"));
        const auto records = reader.read_from(1);
        require(records.size() == expected.size(),
                "logged event count: expected 6, got " + std::to_string(records.size()));
        Events payloads;
        for (std::size_t i = 0; i < records.size(); ++i) {
            require(records[i].seq == i + 1, "log seq must increase from 1");
            require(records[i].wall_time_unix_ns == 123456789, "log event timestamp");
            payloads.push_back(records[i].payload);
        }
        expect_events(payloads, expected);
        require(sleeps == 12, "logged pause-loop sleeps");
    });
    return suite.result();
}
