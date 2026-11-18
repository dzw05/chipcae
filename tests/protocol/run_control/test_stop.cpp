#include "test_support.hpp"

using namespace runctl_test;

int main() {
    Suite suite;
    suite.run("STOP rejects all remaining commands before capability checks", [] {
        TempDirectory dir;
        append(dir.file(), 1, "STOP");
        append(dir.file(), 2, "PAUSE");
        append(dir.file(), 3, "WRITE_OUTPUT_NOW");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {}, collect(events), no_wait());
        BoundaryDecision expected;
        expected.stop = true;
        const WindowMark mark{31, 6.25};
        require(control.at_boundary(mark) == expected, "STOP must set only stop=true");
        require(!control.paused(), "STOP must not pause");
        expect_events(
            events,
            {ack(1, 1, mark), ack(2, 3, mark, "run stopping"), ack(3, 3, mark, "run stopping")});
    });
    suite.run("STOP during pause ends loop without running State", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControl* observed = nullptr;
        RunControlOptions options;
        options.sleep = [&](auto duration) {
            require(duration == 200ms && observed->paused(), "STOP sleep state");
            require(++sleeps == 1, "STOP must exit loop");
            append(dir.file(), 2, "STOP");
        };
        RunControl control(channel, {}, collect(events), options);
        observed = &control;
        BoundaryDecision expected;
        expected.stop = true;
        const WindowMark mark{32, 7.5};
        require(control.at_boundary(mark) == expected, "paused STOP decision");
        require(sleeps == 1 && !control.paused(), "STOP clears pause");
        expect_events(events,
                      {ack(1, 1, mark), state(State::Value::paused, mark), ack(2, 1, mark)});
    });
    suite.run("unsupported checkpoint does not stop subsequent commands", [] {
        TempDirectory dir;
        append(dir.file(), 1, "CHECKPOINT_AND_STOP");
        append(dir.file(), 2, "RESUME");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {}, collect(events), no_wait());
        const WindowMark mark{33, 8.75};
        require(control.at_boundary(mark) == BoundaryDecision{}, "unsupported checkpoint decision");
        expect_events(events, {ack(1, 3, mark, "unsupported"), ack(2, 2, mark)});
    });
    suite.run("supported checkpoint stops batch without setting stop", [] {
        TempDirectory dir;
        append(dir.file(), 1, "CHECKPOINT_AND_STOP");
        append(dir.file(), 2, "STOP");
        append(dir.file(), 3, "PAUSE");
        append(dir.file(), 4, "WRITE_OUTPUT_NOW");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {true, true, true}, collect(events), no_wait());
        BoundaryDecision expected;
        expected.checkpoint_and_stop = true;
        const WindowMark mark{34, 9.0};
        require(control.at_boundary(mark) == expected, "checkpoint must set only checkpoint flag");
        require(!control.paused(), "checkpoint must not pause");
        expect_events(events,
                      {ack(1, 1, mark),
                       ack(2, 3, mark, "run stopping"),
                       ack(3, 3, mark, "run stopping"),
                       ack(4, 3, mark, "run stopping")});
    });
    suite.run("checkpoint during pause ends loop without running State", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControlOptions options;
        options.sleep = [&](auto) {
            require(++sleeps == 1, "checkpoint must exit pause loop");
            append(dir.file(), 2, "CHECKPOINT_AND_STOP");
        };
        RunControl control(channel, {false, false, true}, collect(events), options);
        BoundaryDecision expected;
        expected.checkpoint_and_stop = true;
        const WindowMark mark{35, 10.25};
        require(control.at_boundary(mark) == expected, "paused checkpoint decision");
        require(sleeps == 1 && !control.paused(), "checkpoint clears pause");
        expect_events(events,
                      {ack(1, 1, mark), state(State::Value::paused, mark), ack(2, 1, mark)});
    });
    return suite.result();
}
