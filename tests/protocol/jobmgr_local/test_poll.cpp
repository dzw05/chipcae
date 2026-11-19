#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "manager_support.hpp"

#include <type_traits>

using namespace jobmgr_test;

static_assert(std::is_same_v<decltype(std::declval<const LocalJobManager&>().Get(jobs::JobRef{})),
                             jobs::Job>);
static_assert(
    std::is_same_v<decltype(std::declval<const LocalJobManager&>().List(jobs::ListJobsRequest{})),
                   jobs::ListJobsResponse>);
static_assert(std::is_same_v<decltype(std::declval<const LocalJobManager&>().ListOutputs(
                                 jobs::ListOutputsRequest{})),
                             jobs::ListOutputsResponse>);
static_assert(std::is_same_v<decltype(std::declval<LocalJobManager&>().poll()), std::size_t>);

int main() {
    Suite suite;
    suite.test("empty poll reaps once and preserves RUNNING bytes", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        const auto before = read(fs::path(job.result_dir()) / "job.pb");
        const auto calls = f.fake->reap_calls;
        require(manager->poll() == 0, "empty reap returns zero");
        require(f.fake->reap_calls == calls + 1, "poll calls reap exactly once");
        require(serialize_job(manager->Get(ref(job))) == serialize_job(job),
                "empty poll mutates state");
        require(read(fs::path(job.result_dir()) / "job.pb") == before, "empty poll rewrites state");
    });
    suite.test("one poll drains two exits and persists both outcomes", [] {
        Fixture f;
        auto manager = f.manager();
        auto first = f.running(*manager);
        auto second = f.running(*manager);
        events(first, {chipcae::io::KernelExit{0, 1, "end_reached"}});
        f.finish(first);
        f.finish(second, 19);
        const auto calls = f.fake->reap_calls;
        require(manager->poll() == 2, "two exit records must return two");
        require(f.fake->reap_calls == calls + 1, "one reap for whole batch");
        first = manager->Get(ref(first));
        second = manager->Get(ref(second));
        completed(first, jobs::COMPLETION_KIND_END_REACHED);
        failure(second, jobs::FAILURE_CATEGORY_MPI, "Solver/launcher exited abnormally", 19, 19);
        persisted(*manager, first);
        persisted(*manager, second);
        require(manager->poll() == 0, "drained queue must be empty");
    });
    suite.test("const reads leave queued exits and RUNNING state untouched", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        events(job, {chipcae::io::KernelExit{0, 1, "end_reached"}});
        f.finish(job);
        const auto calls = f.fake->reap_calls;
        const LocalJobManager& reader = *manager;
        require(reader.Get(ref(job)).state() == jobs::JOB_STATE_RUNNING,
                "Get advanced process state");
        require(reader.List({}).jobs(0).state() == jobs::JOB_STATE_RUNNING,
                "List advanced process state");
        reader.ListOutputs(outputs_request(job));
        require(f.fake->reap_calls == calls, "const reads called reap");
        persisted(reader, job);
        require(manager->poll() == 1, "explicit poll consumes queued exit");
        const auto done = reader.Get(ref(job));
        completed(done, jobs::COMPLETION_KIND_END_REACHED);
        persisted(reader, done);
    });
    suite.test("Cancel polls first and does not terminate a completed child", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        events(job, {chipcae::io::KernelExit{0, 1, "user_stop"}});
        f.finish(job);
        const auto calls = f.fake->reap_calls;
        const auto done = manager->Cancel(cancel_request(job, 30));
        completed(done, jobs::COMPLETION_KIND_USER_STOP);
        require(f.fake->reap_calls == calls + 1, "Cancel must poll once first");
        require(f.fake->terminations.empty(), "Cancel terminated an exited child");
        persisted(*manager, done);
    });
    suite.test("Control polls first and rejects newly completed child", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        events(job, {chipcae::io::KernelExit{0, 1, "end_reached"}});
        f.finish(job);
        const auto calls = f.fake->reap_calls;
        const auto reply = manager->Control(f.control(job));
        require(f.fake->reap_calls == calls + 1, "Control must poll once first");
        require(reply.delivery() == jobs::CONTROL_DELIVERY_REJECTED,
                "Control accepted completed child");
        require(reply.message() == "State/action/interval does not permit control",
                "control rejection text");
        require(read(fs::path(job.result_dir()) / "control.jsonl").empty(),
                "rejected control appended");
        const auto done = manager->Get(ref(job));
        completed(done, jobs::COMPLETION_KIND_END_REACHED);
        persisted(*manager, done);
    });
    suite.test("duplicate process exits cannot rewrite terminal Job", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        f.finish(job, 6);
        manager->poll();
        const auto failed = manager->Get(ref(job));
        failure(failed, jobs::FAILURE_CATEGORY_MPI, "Solver/launcher exited abnormally", 6, 6);
        const auto bytes = read(fs::path(job.result_dir()) / "job.pb");
        f.finish(job, 0);
        require(manager->poll() == 1, "duplicate exit record is consumed");
        require(serialize_job(manager->Get(ref(job))) == serialize_job(failed),
                "duplicate exit mutated state");
        require(read(fs::path(job.result_dir()) / "job.pb") == bytes,
                "duplicate exit rewrote bytes");
    });
    suite.test("destruction requests termination only for still owned children", [] {
        Fixture f;
        auto manager = f.manager();
        const auto live = f.running(*manager);
        const auto done = f.running(*manager);
        f.finish(done, 3);
        manager->poll();
        manager.reset();
        require(f.fake->terminations.size() == 1 && f.fake->terminations[0].first == live.run_id(),
                "destructor must terminate live child only");
        auto next = f.manager();
        failure(next->Get(ref(live)),
                jobs::FAILURE_CATEGORY_JOB_MANAGER,
                "Launcher restarted without process ownership");
    });
    return suite.result();
}
#else
int main() {
    std::cout << "SKIP: LocalJobManager not built\n";
    return 77;
}
#endif
