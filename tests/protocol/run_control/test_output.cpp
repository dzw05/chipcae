#include "test_support.hpp"

#include <array>

using namespace runctl_test;

int main() {
    Suite suite;
    suite.run("disabled capabilities reject output commands including invalid interval", [] {
        TempDirectory dir;
        append(dir.file(), 1, "WRITE_OUTPUT_NOW");
        append(dir.file(), 2, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":10");
        append(dir.file(), 3, "SET_OUTPUT_INTERVAL");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {}, collect(events), no_wait());
        const WindowMark mark{41, 11.5};
        require(control.at_boundary(mark) == BoundaryDecision{}, "unsupported output decision");
        expect_events(events,
                      {ack(1, 3, mark, "unsupported"),
                       ack(2, 3, mark, "unsupported"),
                       ack(3, 3, mark, "unsupported")});
    });
    suite.run("enabled output commands keep last interval and write flag", [] {
        TempDirectory dir;
        append(dir.file(), 1, "WRITE_OUTPUT_NOW");
        append(dir.file(), 2, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":10");
        append(dir.file(), 3, "SET_OUTPUT_INTERVAL", ",\"output_interval_time\":0.5");
        append(dir.file(), 4, "WRITE_OUTPUT_NOW");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {true, true, true}, collect(events), no_wait());
        BoundaryDecision expected;
        expected.write_output_now = true;
        expected.output_interval_time = 0.5;
        const WindowMark mark{42, 12.25};
        require(control.at_boundary(mark) == expected, "last time interval clears steps optional");
        expect_events(events, {ack(1, 1, mark), ack(2, 1, mark), ack(3, 1, mark), ack(4, 1, mark)});
        require(control.at_boundary(mark) == BoundaryDecision{}, "output flags reset per boundary");
        expect_events(events, {ack(1, 1, mark), ack(2, 1, mark), ack(3, 1, mark), ack(4, 1, mark)});
    });
    suite.run("steps interval clears prior time optional", [] {
        TempDirectory dir;
        append(dir.file(), 1, "SET_OUTPUT_INTERVAL", ",\"output_interval_time\":0.5");
        append(dir.file(), 2, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":10");
        ControlChannel channel(dir.file());
        Events events;
        RunControl control(channel, {true, false, false}, collect(events), no_wait());
        BoundaryDecision expected;
        expected.output_interval_steps = 10;
        const WindowMark mark{43, 13.75};
        require(control.at_boundary(mark) == expected, "last steps interval clears time optional");
        expect_events(events, {ack(1, 1, mark), ack(2, 1, mark)});
    });
    // Non-finite values are injected into real ControlChannel MPI payloads in mpi_run_control.cpp:
    // the existing file reader does not parse NaN/Inf as interval_time values.
    const std::array<std::pair<const char*, const char*>, 5> invalid{{
        {"zero steps", ",\"output_interval_steps\":0"},
        {"zero time", ",\"output_interval_time\":0"},
        {"negative time", ",\"output_interval_time\":-1"},
        {"both parameters", ",\"output_interval_steps\":10,\"output_interval_time\":0.5"},
        {"neither parameter", ""},
    }};
    for (const auto& [name, parameters] : invalid) {
        suite.run(name, [&] {
            TempDirectory dir;
            append(dir.file(), 1, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":7");
            append(dir.file(), 2, "SET_OUTPUT_INTERVAL", parameters);
            ControlChannel channel(dir.file());
            Events events;
            RunControl control(channel, {true, false, false}, collect(events), no_wait());
            BoundaryDecision expected;
            expected.output_interval_steps = 7;
            const WindowMark mark{44, 14.5};
            require(control.at_boundary(mark) == expected,
                    "invalid interval overwrote valid steps");
            expect_events(events, {ack(1, 1, mark), ack(2, 3, mark, "invalid interval")});
        });
    }
    return suite.result();
}
