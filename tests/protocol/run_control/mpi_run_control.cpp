#include "test_support.hpp"

#include <array>
#include <bit>
#include <limits>
#include <mpi.h>
#include <optional>

using namespace runctl_test;

namespace {
// The real file reader cannot represent non-finite doubles. Change only the last double in
// one real broadcast payload, on every rank (including root), using MPI's profiling interface.
std::optional<double> injected_interval;
int injected_payloads = 0;

void collective_require(bool ok, const std::string& why) {
    int bad = ok ? 0 : 1;
    int any_bad = 0;
    MPI_Allreduce(&bad, &any_bad, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    require(any_bad == 0, why);
}

void expect_root_events(int rank, const Events& actual, const Events& expected) {
    bool ok = true;
    if (rank == 0) {
        try {
            expect_events(actual, expected);
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            ok = false;
        }
    } else {
        ok = actual.empty();
    }
    collective_require(ok, "root event sequence / nonzero-rank emit count");
}

void expect_decision(int rank, const BoundaryDecision& actual, const BoundaryDecision& expected) {
    std::array<int, 5> flags{};
    std::uint64_t steps = 0;
    double time = 0;
    if (rank == 0) {
        flags = {actual.stop,
                 actual.checkpoint_and_stop,
                 actual.write_output_now,
                 actual.output_interval_steps.has_value(),
                 actual.output_interval_time.has_value()};
        steps = actual.output_interval_steps.value_or(0);
        time = actual.output_interval_time.value_or(0);
    }
    MPI_Bcast(flags.data(), static_cast<int>(flags.size()), MPI_INT, 0, MPI_COMM_WORLD);
    MPI_Bcast(&steps, 1, MPI_UINT64_T, 0, MPI_COMM_WORLD);
    MPI_Bcast(&time, 1, MPI_DOUBLE, 0, MPI_COMM_WORLD);
    BoundaryDecision reference;
    reference.stop = flags[0] != 0;
    reference.checkpoint_and_stop = flags[1] != 0;
    reference.write_output_now = flags[2] != 0;
    if (flags[3])
        reference.output_interval_steps = steps;
    if (flags[4])
        reference.output_interval_time = time;
    // Compare every field, including both optional presences and values, then MPI_MAX.
    collective_require(actual == reference, "BoundaryDecision differs from rank 0");
    collective_require(actual == expected,
                       "BoundaryDecision differs from expected command effects");
}

std::string shared_path(int rank, const TempDirectory& dir) {
    std::string path = rank == 0 ? dir.file().string() : std::string{};
    int length = static_cast<int>(path.size());
    MPI_Bcast(&length, 1, MPI_INT, 0, MPI_COMM_WORLD);
    path.resize(static_cast<std::size_t>(length));
    MPI_Bcast(path.data(), length, MPI_CHAR, 0, MPI_COMM_WORLD);
    return path;
}
} // namespace

extern "C" int MPI_Bcast(void* buffer, int count, MPI_Datatype datatype, int root, MPI_Comm comm) {
    const int result = PMPI_Bcast(buffer, count, datatype, root, comm);
    if (result == MPI_SUCCESS && injected_interval && datatype == MPI_BYTE && count > 8) {
        auto* bytes = static_cast<std::uint8_t*>(buffer);
        const auto bits = std::bit_cast<std::uint64_t>(*injected_interval);
        for (unsigned i = 0; i < 8; ++i)
            bytes[count - 8 + static_cast<int>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
        ++injected_payloads;
        injected_interval.reset();
    }
    return result;
}

int main(int argc, char** argv) {
    MPI_Init(&argc, &argv);
    int rank = 0;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    int result = 1;
    try {
        Suite suite;
        suite.run("collective pause exits after three sleeps; next window STOP", [&] {
            TempDirectory dir;
            const auto path = shared_path(rank, dir);
            if (rank == 0)
                append(path, 1, "PAUSE");
            MPI_Barrier(MPI_COMM_WORLD);
            ControlChannel channel(path);
            Events events;
            int sleeps = 0;
            RunControl* observed = nullptr;
            RunControlOptions options;
            options.sleep = [&](auto duration) {
                if (duration != 200ms || !observed->paused() || ++sleeps > 3)
                    MPI_Abort(MPI_COMM_WORLD, 2);
                if (rank == 0 && sleeps == 3)
                    append(path, 2, "RESUME");
            };
            RunControl control(channel, {}, collect(events), options);
            observed = &control;
            const WindowMark first{61, 16.5};
            const auto paused_decision = control.at_boundary(first);
            collective_require(sleeps == 3, "expected exactly 3 sleeps on every rank");
            collective_require(!control.paused(), "resume clears paused on every rank");
            expect_decision(rank, paused_decision, {});
            expect_root_events(rank,
                               events,
                               {ack(1, 1, first),
                                state(State::Value::paused, first),
                                ack(2, 1, first),
                                state(State::Value::running, first)});
            if (rank == 0)
                append(path, 3, "STOP");
            MPI_Barrier(MPI_COMM_WORLD);
            BoundaryDecision stop;
            stop.stop = true;
            const WindowMark second{62, 17.75};
            expect_decision(rank, control.at_boundary(second), stop);
            collective_require(!control.paused() && sleeps == 3, "STOP must not wait or pause");
            expect_root_events(rank,
                               events,
                               {ack(1, 1, first),
                                state(State::Value::paused, first),
                                ack(2, 1, first),
                                state(State::Value::running, first),
                                ack(3, 1, second)});
        });
        suite.run("collective decisions include nondefault optional values", [&] {
            TempDirectory dir;
            const auto path = shared_path(rank, dir);
            if (rank == 0) {
                append(path, 1, "WRITE_OUTPUT_NOW");
                append(path, 2, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":10");
                append(path, 3, "SET_OUTPUT_INTERVAL", ",\"output_interval_time\":0.5");
            }
            MPI_Barrier(MPI_COMM_WORLD);
            ControlChannel channel(path);
            Events events;
            RunControl control(channel, {true, true, true}, collect(events), no_wait());
            BoundaryDecision expected;
            expected.write_output_now = true;
            expected.output_interval_time = 0.5;
            const WindowMark first{63, 18.25};
            expect_decision(rank, control.at_boundary(first), expected);
            expect_root_events(
                rank, events, {ack(1, 1, first), ack(2, 1, first), ack(3, 1, first)});
            if (rank == 0) {
                append(path, 4, "SET_OUTPUT_INTERVAL", ",\"output_interval_steps\":7");
                append(path, 5, "CHECKPOINT_AND_STOP");
            }
            MPI_Barrier(MPI_COMM_WORLD);
            expected = {};
            expected.checkpoint_and_stop = true;
            expected.output_interval_steps = 7;
            const WindowMark second{64, 19.5};
            expect_decision(rank, control.at_boundary(second), expected);
            expect_root_events(rank,
                               events,
                               {ack(1, 1, first),
                                ack(2, 1, first),
                                ack(3, 1, first),
                                ack(4, 1, second),
                                ack(5, 1, second)});
        });
        suite.run("rank-local mark mismatch propagates runtime_error to every rank", [&] {
            TempDirectory dir;
            ControlChannel channel(shared_path(rank, dir));
            Events events;
            RunControl control(channel, {}, collect(events), no_wait());
            bool caught = false;
            try {
                (void)control.at_boundary({65, rank == 0 ? 20.25 : 20.5});
            } catch (const std::runtime_error& error) {
                caught = std::string(error.what()) == "control channel window mismatch";
            }
            collective_require(caught, "missing unchanged collective mark mismatch exception");
            expect_root_events(rank, events, {});
        });
        const std::array<std::pair<const char*, double>, 3> nonfinite{{
            {"NaN interval rejected", std::numeric_limits<double>::quiet_NaN()},
            {"positive infinity interval rejected", std::numeric_limits<double>::infinity()},
            {"negative infinity interval rejected", -std::numeric_limits<double>::infinity()},
        }};
        for (const auto& [name, value] : nonfinite) {
            suite.run(name, [&] {
                TempDirectory dir;
                const auto path = shared_path(rank, dir);
                if (rank == 0)
                    append(path, 1, "SET_OUTPUT_INTERVAL", ",\"output_interval_time\":0.5");
                MPI_Barrier(MPI_COMM_WORLD);
                ControlChannel channel(path);
                Events events;
                RunControl control(channel, {true, false, false}, collect(events), no_wait());
                injected_payloads = 0;
                injected_interval = value;
                const WindowMark mark{66, 21.75};
                const auto decision = control.at_boundary(mark);
                injected_interval.reset();
                collective_require(
                    injected_payloads == 1,
                    "expected non-finite value injected into one real command payload");
                expect_decision(rank, decision, {});
                expect_root_events(rank, events, {ack(1, 3, mark, "invalid interval")});
            });
        }
        result = suite.result();
    } catch (const std::exception& error) {
        std::cerr << "rank " << rank << ": " << error.what() << '\n';
        MPI_Abort(MPI_COMM_WORLD, 3);
    }
    int any_failed = 0;
    MPI_Allreduce(&result, &any_failed, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    MPI_Finalize();
    return any_failed;
}
