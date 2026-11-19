#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

#include <sys/wait.h>
#include <unistd.h>

using namespace jobmgr_test;

namespace {
jobs::Job seed_job(Fixture& f, jobs::JobState state = jobs::JOB_STATE_COMPLETED) {
    jobs::Job job;
    job.set_run_id(f.id());
    job.set_client_request_id(f.id());
    job.set_name("persisted synthetic job");
    job.set_state(state);
    job.set_attempt(1);
    job.set_result_dir((f.config.run_root / job.run_id()).string());
    job.set_result_dir_local(true);
    job.mutable_submitted_at()->set_seconds(1);
    job.mutable_started_at()->set_seconds(2);
    if (state == jobs::JOB_STATE_COMPLETED) {
        job.mutable_ended_at()->set_seconds(3);
        job.set_completion(jobs::COMPLETION_KIND_END_REACHED);
    }
    write(fs::path(job.result_dir()) / "job.pb", job.SerializeAsString());
    write(fs::path(job.result_dir()) / "control.jsonl", "");
    return job;
}
std::string line(std::string_view sequence, std::string_view id) {
    return "{\"issuedAt\":\"2026-01-01T00:00:00.000Z\",\"cmd_seq\":\"" + std::string(sequence) +
           "\",\"client_request_id\":\"" + std::string(id) + "\",\"action\":\"PAUSE\"}\n";
}
} // namespace

int main() {
    Suite suite;
    suite.test("restart fails RUNNING, preserves COMPLETED bytes and Submit retries", [] {
        Fixture f;
        const auto first_request = f.request();
        const auto second_request = f.request();
        jobs::Job done, live;
        std::string done_bytes, live_snapshot;
        {
            auto manager = f.manager();
            done = manager->Submit(first_request, f.case_path, f.profile_path);
            require(done.state() == jobs::JOB_STATE_RUNNING, "first run starts");
            live = manager->Submit(second_request, f.case_path, f.profile_path);
            require(live.state() == jobs::JOB_STATE_RUNNING, "second run starts");
            events(done, {chipcae::io::KernelExit{0, 1, "end_reached"}});
            f.finish(done);
            manager->poll();
            done = manager->Get(ref(done));
            completed(done, jobs::COMPLETION_KIND_END_REACHED);
            persisted(*manager, done);
            persisted(*manager, live);
            done_bytes = read(fs::path(done.result_dir()) / "job.pb");
            live_snapshot = read(fs::path(live.result_dir()) / "inputs/case.json");
        }
        const auto starts = f.fake->starts.size();
        write(f.case_path, "changed invalid source after restart");
        auto recovered = f.manager();
        const auto failed = recovered->Get(ref(live));
        failure(failed,
                jobs::FAILURE_CATEGORY_JOB_MANAGER,
                "Launcher restarted without process ownership");
        persisted(*recovered, failed);
        require(serialize_job(recovered->Get(ref(done))) == serialize_job(done),
                "COMPLETED recovery changes Job");
        require(read(fs::path(done.result_dir()) / "job.pb") == done_bytes,
                "COMPLETED recovery rewrites bytes");
        require(serialize_job(recovered->Submit(first_request, f.case_path, f.profile_path)) ==
                    serialize_job(done),
                "completed Submit retry after restart");
        require(serialize_job(recovered->Submit(second_request, f.case_path, f.profile_path)) ==
                    serialize_job(failed),
                "failed Submit retry after restart");
        require(directory_count(f.config.run_root) == 2 && f.fake->starts.size() == starts,
                "recovered Submit retry starts process or creates directory");
        require(read(fs::path(live.result_dir()) / "inputs/case.json") == live_snapshot,
                "recovered retry resnapshots inputs");
    });
    suite.test("accepted controls and sequence replies survive restart without truncation", [] {
        Fixture f;
        jobs::Job job;
        jobs::ControlRequest first, second;
        jobs::ControlReply reply1, reply2;
        std::string bytes;
        {
            auto manager = f.manager();
            job = f.running(*manager);
            first = f.control(job);
            second = f.control(job, jobs::CONTROL_ACTION_RESUME);
            reply1 = manager->Control(first);
            reply2 = manager->Control(second);
            require(reply1.cmd_seq() == 1 && reply2.cmd_seq() == 2,
                    "control sequences before restart");
            bytes = read(fs::path(job.result_dir()) / "control.jsonl");
        }
        auto recovered = f.manager();
        require(recovered->Control(first).SerializeAsString() == reply1.SerializeAsString(),
                "first recovered reply");
        require(recovered->Control(second).SerializeAsString() == reply2.SerializeAsString(),
                "second recovered reply");
        const auto rejected = recovered->Control(f.control(job));
        require(rejected.delivery() == jobs::CONTROL_DELIVERY_REJECTED && rejected.cmd_seq() == 0,
                "recovered FAILED job must reject new controls");
        require(read(fs::path(job.result_dir()) / "control.jsonl") == bytes,
                "recovery/control retry truncates or appends");
    });
    for (const auto state :
         {jobs::JOB_STATE_SUBMITTING, jobs::JOB_STATE_STARTING, jobs::JOB_STATE_RUNNING})
        suite.test("recovery fails nonterminal " + jobs::JobState_Name(state), [&] {
            Fixture f;
            const auto job = seed_job(f, state);
            auto manager = f.manager();
            const auto recovered = manager->Get(ref(job));
            failure(recovered,
                    jobs::FAILURE_CATEGORY_JOB_MANAGER,
                    "Launcher restarted without process ownership");
            persisted(*manager, recovered);
            require(f.fake->starts.empty() && f.fake->terminations.empty(),
                    "recovered job must not gain process ownership");
        });
    suite.test("uint64 journal sequences preserve precision and maximum", [] {
        Fixture f;
        const auto job = seed_job(f);
        const auto first = f.id(), last = f.id();
        const auto path = fs::path(job.result_dir()) / "control.jsonl";
        const auto bytes = line("9007199254740993", first) + line("18446744073709551615", last);
        write(path, bytes);
        auto manager = f.manager();
        auto request = f.control(job);
        request.set_client_request_id(first);
        auto reply = manager->Control(request);
        require(reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED &&
                    reply.cmd_seq() == 9007199254740993ULL,
                "recovered 53-bit sequence");
        request.set_client_request_id(last);
        reply = manager->Control(request);
        require(reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED &&
                    reply.cmd_seq() == 18446744073709551615ULL,
                "recovered uint64 maximum");
        require(read(path) == bytes, "journal recovery rewrites bytes");
    });
    suite.test("incomplete journal requires repair and failed construction releases lock", [] {
        Fixture f;
        const auto job = seed_job(f);
        auto bytes = line("1", f.id());
        bytes.pop_back();
        const auto path = fs::path(job.result_dir()) / "control.jsonl";
        write(path, bytes);
        throws_message([&] { auto manager = f.manager(); },
                       "Incomplete broker journal; repair required before append");
        require(read(path) == bytes, "bad journal changed during failure");
        write(path, bytes + '\n');
        auto manager = f.manager();
        require(manager->Get(ref(job)).state() == jobs::JOB_STATE_COMPLETED,
                "owned lock leaked on failed construction");
    });
    for (const auto& [name, sequences] :
         std::vector<std::pair<std::string, std::vector<std::string>>>{
             {"duplicate", {"1", "1"}},
             {"decreasing", {"2", "1"}},
             {"zero", {"0"}},
             {"negative", {"-1"}},
             {"nonnumeric", {"abc"}},
             {"fractional", {"1.5"}},
             {"overflow", {"18446744073709551616"}}})
        suite.test("invalid journal sequence " + name, [&] {
            Fixture f;
            const auto job = seed_job(f);
            std::string bytes;
            for (const auto& seq : sequences)
                bytes += line(seq, f.id());
            const auto path = fs::path(job.result_dir()) / "control.jsonl";
            write(path, bytes);
            throws_message([&] { auto manager = f.manager(); }, "Invalid broker journal sequence");
            require(read(path) == bytes, "invalid sequence journal changed");
        });
    suite.test("corrupt protobuf is rejected and lock released after failed construction", [] {
        Fixture f;
        const auto job = seed_job(f);
        const auto path = fs::path(job.result_dir()) / "job.pb";
        write(path, std::string("\xff\xff", 2));
        throws_message([&] { auto manager = f.manager(); }, "Invalid persisted Job");
        write(path, job.SerializeAsString());
        auto manager = f.manager();
        require(serialize_job(manager->Get(ref(job))) == serialize_job(job),
                "failed recovery leaked owned lock");
    });
    suite.test("directories without job.pb are ignored", [] {
        Fixture f;
        fs::create_directories(f.config.run_root / "orphan-inputs");
        const auto job = seed_job(f);
        auto manager = f.manager();
        const auto list = manager->List({});
        require(list.jobs_size() == 1 && list.jobs(0).run_id() == job.run_id(),
                "missing-job directory was not ignored");
    });
    suite.test("live root lock excludes second manager and permits reacquisition after destruction",
               [] {
                   Fixture f;
                   auto first = f.manager();
                   throws_message([&] { auto second = f.manager(); },
                                  "Local job root already has a control writer");
                   const auto lock = f.config.run_root / "launcher.lock";
                   std::istringstream content(read(lock));
                   std::string pid, app, host;
                   require(static_cast<bool>(std::getline(content, pid)) &&
                               static_cast<bool>(std::getline(content, app)) &&
                               static_cast<bool>(std::getline(content, host)),
                           "QLockFile-compatible three lines");
                   require(pid == std::to_string(::getpid()) && !app.empty() && !host.empty(),
                           "lock owner metadata");
                   first.reset();
                   require(!fs::exists(lock), "destruction leaves lock");
                   auto third = f.manager();
                   require(fs::exists(lock), "reacquisition did not create lock");
               });
    suite.test("demonstrably dead same-host PID lock is reclaimed", [] {
        Fixture f;
        const pid_t child = ::fork();
        require(child >= 0, "cannot create dead PID fixture");
        if (child == 0)
            ::_exit(0);
        int status = 0;
        require(::waitpid(child, &status, 0) == child && WIFEXITED(status),
                "cannot reap dead PID fixture");
        char host[256]{};
        require(::gethostname(host, sizeof(host) - 1) == 0, "cannot read hostname");
        const auto lock = f.config.run_root / "launcher.lock";
        write(lock, std::to_string(child) + "\nchipcae-jobmgr\n" + host + "\n");
        auto manager = f.manager();
        require(read(lock).starts_with(std::to_string(::getpid()) + '\n'),
                "dead PID was not replaced with current owner");
    });
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: LocalJobManager not built\n";
    return 77;
}
#endif
