#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "launch_support.hpp"

using namespace jobmgr_test;
namespace detail = chipcae::jobmgr::detail;

int main() {
    Suite suite;
    for (const auto mode : {"sleep", "ignore-term"}) {
        suite.test(std::string("Cancel 200ms terminates and reaps ") + mode, [&] {
            LaunchFixture f;
            f.mode(mode);
            LocalJobManager manager(f.config);
            const auto job = f.launch(manager);
            f.captured(job); // ignore-term has installed its handler before publishing readiness.
            auto request = cancel_request(job);
            request.mutable_grace()->set_nanos(200000000);
            const auto start = Clock::now();
            const auto result = manager.Cancel(request);
            require(Clock::now() - start < 500ms, "Cancel did not return promptly");
            cancelled(manager, result);
            reap_until_gone(
                manager, f.child.pid, std::string_view(mode) == "ignore-term" ? 2200ms : 2000ms);
            const auto elapsed = Clock::now() - start;
            if (std::string_view(mode) == "ignore-term")
                require(elapsed >= 200ms && elapsed < 2200ms,
                        "SIGKILL did not honor grace deadline");
            for (int i = 0; i < 3; ++i) {
                manager.poll();
                cancelled(manager, manager.Get(ref(job)));
            }
            cancelled(manager, manager.wait(job.run_id(), 10s));
        });
    }
    suite.test("watchdog kills without poll and leaves zombie until reap", [] {
        LaunchFixture f;
        f.mode("ignore-term");
        LocalJobManager manager(f.config);
        const auto job = f.launch(manager);
        f.captured(job);
        auto request = cancel_request(job);
        request.mutable_grace()->set_nanos(200000000);
        cancelled(manager, manager.Cancel(request));
        // No manager call during grace + 500ms: only the watchdog can kill it.
        std::this_thread::sleep_for(700ms);
        require(process_state(f.child.pid) == 'Z', "watchdog did not kill independently of poll");
        require(!gone(f.child.pid), "child reaped outside manager poll");
        reap_until_gone(manager, f.child.pid, 2s);
        cancelled(manager, manager.Get(ref(job)));
    });
    suite.test("real zero grace and cancelled wait return immediately", [] {
        LaunchFixture f;
        f.mode("ignore-term");
        LocalJobManager manager(f.config);
        const auto job = f.launch(manager);
        f.captured(job);
        const auto start = Clock::now();
        cancelled(manager, manager.Cancel(cancel_request(job)));
        cancelled(manager, manager.wait(job.run_id(), 10s));
        require(Clock::now() - start < 500ms, "cancelled wait blocked on child reap");
        reap_until_gone(manager, f.child.pid, 2s);
        for (int i = 0; i < 3; ++i) {
            manager.poll();
            cancelled(manager, manager.Get(ref(job)));
        }
    });
    suite.test("destruction kills and reaps before root can be reopened", [] {
        LaunchFixture f;
        f.mode("sleep");
        jobs::Job job;
        {
            LocalJobManager manager(f.config);
            job = f.launch(manager);
            f.captured(job);
        }
        require(gone(f.child.pid), "manager destruction left live or zombie child");
        int status = 0;
        errno = 0;
        require(waitpid(f.child.pid, &status, WNOHANG) == -1 && errno == ECHILD,
                "manager destruction left a waitable child");
        LocalJobManager recovered(f.config);
        const auto result = recovered.Get(ref(job));
        failure(result,
                jobs::FAILURE_CATEGORY_JOB_MANAGER,
                "Launcher restarted without process ownership");
        persisted(recovered, result);
    });
    suite.test("wait 100ms times out with nonterminal persisted job", [] {
        LaunchFixture f;
        f.mode("sleep");
        LocalJobManager manager(f.config);
        const auto job = f.launch(manager);
        f.captured(job);
        const auto start = Clock::now();
        const auto result = manager.wait(job.run_id(), 100ms);
        const auto elapsed = Clock::now() - start;
        require(elapsed >= 100ms && elapsed < 2100ms, "wait returned before timeout or too late");
        require(!detail::terminal(result.state()), "sleeper wait returned terminal state");
        persisted(manager, result);
        cancelled(manager, manager.Cancel(cancel_request(job)));
        reap_until_gone(manager, f.child.pid, 2s);
    });
    suite.test("wait honors positive timeout while no exits are available", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        const auto before = f.fake->reap_calls;
        const auto start = Clock::now();
        const auto result = manager->wait(job.run_id(), 100ms);
        const auto elapsed = Clock::now() - start;
        require(elapsed >= 100ms && elapsed < 2100ms,
                "wait returned before positive timeout or too late");
        require(f.fake->reap_calls > before + 1, "positive wait did not keep polling");
        require(!detail::terminal(result.state()), "wait made a pending run terminal");
        persisted(*manager, result);
    });
    for (const auto timeout : {0ms, -1ms}) {
        suite.test("wait " + std::to_string(timeout.count()) + "ms polls exactly once", [&] {
            Fixture f;
            auto manager = f.manager();
            const auto job = f.running(*manager);
            const auto before = f.fake->reap_calls;
            const auto start = Clock::now();
            const auto result = manager->wait(job.run_id(), timeout);
            require(f.fake->reap_calls == before + 1,
                    "nonpositive wait must invoke reap exactly once");
            require(Clock::now() - start < 500ms, "nonpositive wait blocked");
            require(!detail::terminal(result.state()), "pending fake became terminal");
            persisted(*manager, result);
        });
    }
    for (const auto timeout : {0ms, -1ms}) {
        suite.test("wait " + std::to_string(timeout.count()) + "ms consumes queued exit", [&] {
            Fixture f;
            auto manager = f.manager();
            const auto job = f.running(*manager);
            events(job, {chipcae::io::KernelHello{}, chipcae::io::KernelExit{0, 2, "end_reached"}});
            f.finish(job);
            const auto before = f.fake->reap_calls;
            const auto result = manager->wait(job.run_id(), timeout);
            require(f.fake->reap_calls == before + 1, "queued exit wait must poll exactly once");
            completed(result, jobs::COMPLETION_KIND_END_REACHED);
            persisted(*manager, result);
            require(manager->poll() == 0, "wait left a consumed exit queued");
        });
    }
    suite.test("terminal wait returns immediately", [] {
        Fixture f;
        auto manager = f.manager();
        const auto job = f.running(*manager);
        cancelled(*manager, manager->Cancel(cancel_request(job)));
        const auto before = f.fake->reap_calls;
        const auto start = Clock::now();
        cancelled(*manager, manager->wait(job.run_id(), 10s));
        require(Clock::now() - start < 500ms, "terminal wait slept until timeout");
        require(f.fake->reap_calls == before + 1, "terminal wait must poll once before returning");
    });
    suite.test("unknown wait preserves exact lookup exception", [] {
        Fixture f;
        auto manager = f.manager();
        try {
            manager->wait("unknown", 100ms);
        } catch (const std::runtime_error& e) {
            require(std::string_view(e.what()) == "Unknown run_id", "wrong unknown wait message");
            return;
        }
        require(false, "unknown wait must throw std::runtime_error");
    });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
