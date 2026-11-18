#include "test_support.hpp"

using namespace runctl_test;

int main() {
    Suite suite;
    suite.run("empty boundary leaves missing control file absent", [] {
        TempDirectory dir;
        ControlChannel channel(dir.file());
        Events events;
        int sleeps = 0;
        RunControlOptions options;
        options.sleep = [&](auto) { ++sleeps; };
        RunControl control(channel, {}, collect(events), options);
        require(!control.paused(), "initial paused state");
        require(control.at_boundary({7, 1.25}) == BoundaryDecision{}, "default empty decision");
        expect_events(events, {});
        require(sleeps == 0, "empty boundary must not sleep");
        require(!std::filesystem::exists(dir.file()), "reader created missing control file");
    });
    suite.run("same-batch pause/resume ack precedes each state", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        append(dir.file(), 2, "RESUME");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {}, collect(events), no_wait());
        const WindowMark mark{7, 1.25};
        require(control.at_boundary(mark) == BoundaryDecision{}, "pause/resume decision");
        require(!control.paused(), "resume clears pause");
        expect_events(events,
                      {ack(1, 1, mark),
                       state(State::Value::paused, mark),
                       ack(2, 1, mark),
                       state(State::Value::running, mark)});
    });
    suite.run("duplicate pause and duplicate resume have NOOP acks", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        append(dir.file(), 2, "PAUSE");
        ControlChannel channel(dir.file());
        Events events;
        RunControlOptions options;
        int sleeps = 0;
        options.sleep = [&](auto duration) {
            require(duration == 200ms, "default pause_poll");
            require(++sleeps == 1, "resume should end first sleep");
            append(dir.file(), 3, "RESUME");
            append(dir.file(), 4, "RESUME");
        };
        RunControl control(channel, {}, collect(events), options);
        const WindowMark mark{8, 2.5};
        require(control.at_boundary(mark) == BoundaryDecision{}, "duplicate decision");
        require(sleeps == 1 && !control.paused(), "duplicate pause loop exit");
        expect_events(events,
                      {ack(1, 1, mark),
                       state(State::Value::paused, mark),
                       ack(2, 2, mark),
                       ack(3, 1, mark),
                       state(State::Value::running, mark),
                       ack(4, 2, mark)});
    });
    suite.run("same-window calls only process newly appended commands", [] {
        TempDirectory dir;
        append(dir.file(), 1, "RESUME");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {false, true, false}, collect(events), no_wait());
        const WindowMark mark{9, 3.5};
        require(control.at_boundary(mark) == BoundaryDecision{}, "unpaused resume decision");
        expect_events(events, {ack(1, 2, mark)});
        append(dir.file(), 2, "WRITE_OUTPUT_NOW");
        BoundaryDecision output;
        output.write_output_now = true;
        require(control.at_boundary(mark) == output, "same-window new command");
        expect_events(events, {ack(1, 2, mark), ack(2, 1, mark)});
        require(control.at_boundary(mark) == BoundaryDecision{}, "decision resets per call");
        expect_events(events, {ack(1, 2, mark), ack(2, 1, mark)});
    });
    suite.run("poll rollback invalid_argument propagates unchanged", [] {
        TempDirectory dir;
        ControlChannel channel(dir.file());
        RunControl control(channel, {}, {}, no_wait());
        require(control.at_boundary({9, 3.5}) == BoundaryDecision{}, "initial boundary");
        bool caught = false;
        try {
            (void)control.at_boundary({8, 3.0});
        } catch (const std::invalid_argument& error) {
            caught = std::string(error.what()) == "control channel window rollback";
        }
        require(caught, "missing unchanged poll rollback exception");
    });
    suite.run("empty emit and default sleep accept same-batch transitions", [] {
        TempDirectory dir;
        append(dir.file(), 1, "PAUSE");
        append(dir.file(), 2, "RESUME");
        append(dir.file(), 3, "STOP");
        ControlChannel channel(dir.file());
        RunControl control(channel, {}, {});
        BoundaryDecision expected;
        expected.stop = true;
        require(control.at_boundary({10, 4.0}) == expected, "empty emit still dispatches STOP");
        require(!control.paused(), "empty emit pause state");
    });
    return suite.result();
}
