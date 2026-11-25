#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "launch_support.hpp"

#include <tuple>

using namespace jobmgr_test;
namespace io = chipcae::io;

namespace {
using Ack = std::tuple<std::uint64_t, std::string, std::uint32_t>;

std::vector<io::Event> read_events(const fs::path& directory) {
    const auto file = directory / "events.bin";
    std::uint64_t offset = 0;
    auto result = io::EventLogReader(file).read_from(0, &offset);
    require(offset == fs::file_size(file), "event reader did not consume all bytes");
    return result;
}

std::size_t journal_lines(const fs::path& directory) {
    const auto text = read(directory / "control.jsonl");
    return static_cast<std::size_t>(std::count(text.begin(), text.end(), '\n'));
}

void diagnostics(const fs::path& directory) {
    try {
        std::cerr << "control.jsonl:\n" << read(directory / "control.jsonl") << '\n';
        const auto bytes = read(directory / "events.bin");
        std::ostringstream hex;
        hex << std::hex << std::setfill('0');
        for (const unsigned char byte : bytes)
            hex << std::setw(2) << static_cast<unsigned>(byte);
        std::cerr << "events.bin raw hex: " << hex.str() << '\n';
        std::uint64_t offset = 0;
        for (const auto& event :
             io::EventLogReader(directory / "events.bin").read_from(0, &offset)) {
            std::cerr << "event seq=" << event.seq << " kind=" << event.payload.index();
            if (const auto* ack = std::get_if<io::ControlAck>(&event.payload))
                std::cerr << " cmd_seq=" << ack->cmd_seq << " UUID=" << ack->client_request_id
                          << " status=" << ack->status;
            if (const auto* exit = std::get_if<io::KernelExit>(&event.payload))
                std::cerr << " exit_code=" << exit->exit_code << " reason=" << exit->reason;
            std::cerr << '\n';
        }
        std::cerr << "decoded offset=" << offset << " bytes=" << bytes.size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "artifact diagnostics: " << error.what() << '\n';
    }
}

template <class F>
void scenario(F&& body, int timeout_ms = 5000) {
    LaunchFixture f;
    f.mode("control");
    auto profile = f.profile();
    profile["x-control-timeout-ms"] = timeout_ms;
    f.profile(profile);
    LocalJobManager manager(f.config);
    jobs::Job launched;
    try {
        launched = f.launch(manager);
        f.ready(launched);
        body(f, manager, launched);
    } catch (...) {
        if (!launched.result_dir().empty())
            diagnostics(launched.result_dir());
        throw;
    }
}

void queued(const jobs::ControlReply& reply, std::uint64_t sequence) {
    require(reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED, "control was not QUEUED");
    require(reply.cmd_seq() == sequence, "wrong queued command sequence");
}

jobs::Job verify(LaunchFixture& f,
                 LocalJobManager& manager,
                 const jobs::Job& launched,
                 jobs::CompletionKind kind,
                 std::string_view reason,
                 std::size_t lines,
                 std::size_t event_count,
                 const std::vector<Ack>& expected) {
    const auto job = manager.wait(launched.run_id(), 10s);
    completed(job, kind);
    require(!job.has_failure(), "completed job has a failure record");
    persisted(manager, job);
    f.captured(job);
    const fs::path directory = job.result_dir();
    require(journal_lines(directory) == lines, "wrong control journal line count");
    const auto events = read_events(directory);
    require(events.size() == event_count, "wrong event count");
    std::uint64_t sequence = 0;
    std::vector<Ack> actual;
    for (const auto& event : events) {
        require(event.seq == ++sequence, "event sequences are not contiguous from 1");
        if (const auto* ack = std::get_if<io::ControlAck>(&event.payload))
            actual.emplace_back(ack->cmd_seq, ack->client_request_id, ack->status);
    }
    require(actual == expected, "ordered acknowledgement sequence/UUID/status differs");
    require(std::holds_alternative<io::KernelHello>(events.front().payload),
            "first event is not KernelHello");
    const auto* exit = std::get_if<io::KernelExit>(&events.back().payload);
    require(exit != nullptr, "last event is not KernelExit");
    require(exit->exit_code == 0, "KernelExit has nonzero exit code");
    require(exit->reason == reason, "wrong KernelExit reason");
    return job;
}
} // namespace

int main() {
    Suite suite;
    suite.test("stop through journal completes USER_STOP", [] {
        scenario([](auto& f, auto& manager, const auto& job) {
            const auto request = f.control(job, jobs::CONTROL_ACTION_STOP);
            queued(manager.Control(request), 1);
            verify(f,
                   manager,
                   job,
                   jobs::COMPLETION_KIND_USER_STOP,
                   "user_stop",
                   1,
                   3,
                   {{1, request.client_request_id(), 1}});
        });
    });
    suite.test("checkpoint_and_stop through journal completes STOP_AFTER_CHECKPOINT", [] {
        scenario([](auto& f, auto& manager, const auto& job) {
            const auto request = f.control(job, jobs::CONTROL_ACTION_CHECKPOINT_AND_STOP);
            queued(manager.Control(request), 1);
            verify(f,
                   manager,
                   job,
                   jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT,
                   "checkpoint_and_stop",
                   1,
                   3,
                   {{1, request.client_request_id(), 1}});
        });
    });
    suite.test("non-stop control is acknowledged rejected before STOP", [] {
        scenario([](auto& f, auto& manager, const auto& job) {
            const auto pause = f.control(job, jobs::CONTROL_ACTION_PAUSE);
            const auto stop = f.control(job, jobs::CONTROL_ACTION_STOP);
            require(pause.client_request_id() != stop.client_request_id(), "requests share UUID");
            queued(manager.Control(pause), 1);
            queued(manager.Control(stop), 2);
            verify(f,
                   manager,
                   job,
                   jobs::COMPLETION_KIND_USER_STOP,
                   "user_stop",
                   2,
                   4,
                   {{1, pause.client_request_id(), 3}, {2, stop.client_request_id(), 1}});
        });
    });
    suite.test("duplicate STOP request id acknowledged once", [] {
        scenario([](auto& f, auto& manager, const auto& job) {
            const auto request = f.control(job, jobs::CONTROL_ACTION_STOP);
            const auto first = manager.Control(request);
            const auto duplicate = manager.Control(request);
            queued(first, 1);
            queued(duplicate, 1);
            require(first.SerializeAsString() == duplicate.SerializeAsString(),
                    "duplicate request did not return identical reply");
            verify(f,
                   manager,
                   job,
                   jobs::COMPLETION_KIND_USER_STOP,
                   "user_stop",
                   1,
                   3,
                   {{1, request.client_request_id(), 1}});
        });
    });
    suite.test("no control reaches END_REACHED", [] {
        scenario(
            [](auto& f, auto& manager, const auto& job) {
                verify(f, manager, job, jobs::COMPLETION_KIND_END_REACHED, "end_reached", 0, 2, {});
            },
            300);
    });
    suite.test("STOP after terminal state is rejected", [] {
        scenario(
            [](auto& f, auto& manager, const auto& job) {
                const auto done = verify(
                    f, manager, job, jobs::COMPLETION_KIND_END_REACHED, "end_reached", 0, 2, {});
                const fs::path directory = done.result_dir();
                const auto event_bytes = read(directory / "events.bin");
                const auto job_bytes = read(directory / "job.pb");
                const auto request = f.control(done, jobs::CONTROL_ACTION_STOP);
                const auto reply = manager.Control(request);
                require(reply.delivery() == jobs::CONTROL_DELIVERY_REJECTED,
                        "terminal job accepted STOP");
                require(reply.cmd_seq() == 0, "rejected STOP has a command sequence");
                require(reply.message() == "State/action/interval does not permit control",
                        "wrong terminal STOP rejection message");
                require(journal_lines(directory) == 0 && read(directory / "control.jsonl").empty(),
                        "terminal STOP changed empty journal");
                require(read(directory / "events.bin") == event_bytes,
                        "terminal STOP changed events");
                require(read(directory / "job.pb") == job_bytes,
                        "terminal STOP changed persisted job");
                require(serialize_job(manager.Get(ref(done))) == serialize_job(done),
                        "terminal STOP changed returned job");
                persisted(manager, done);
            },
            300);
    });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
