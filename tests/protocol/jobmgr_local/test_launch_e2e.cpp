#include "test_support.hpp"

#if CHIPCAE_JOBMGR_LOCAL
#include "launch_support.hpp"

using namespace jobmgr_test;
namespace detail = chipcae::jobmgr::detail;

int main() {
    Suite suite;
    for (const auto& [reason, kind] : std::vector<std::pair<std::string, jobs::CompletionKind>>{
             {"end_reached", jobs::COMPLETION_KIND_END_REACHED},
             {"user_stop", jobs::COMPLETION_KIND_USER_STOP},
             {"checkpoint_and_stop", jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT},
             {"error", jobs::COMPLETION_KIND_END_REACHED}}) {
        suite.test("direct success " + reason, [&] {
            LaunchFixture f;
            f.mode("success", reason);
            LocalJobManager manager(f.config);
            const auto launched = f.launch(manager);
            const auto job = manager.wait(launched.run_id(), 10s);
            require(detail::terminal(job.state()), "wait did not reach terminal state");
            completed(job, kind);
            require(!job.has_failure(), "successful job has a failure record");
            persisted(manager, job);
            f.captured(job);
        });
    }
    for (const auto mode :
         {"missing", "torn", "invalid", "failed-exit", "no-exit", "abnormal", "crash"}) {
        suite.test(std::string("direct outcome ") + mode, [&] {
            LaunchFixture f;
            f.mode(mode);
            LocalJobManager manager(f.config);
            const auto launched = f.launch(manager);
            const auto job = manager.wait(launched.run_id(), 10s);
            require(detail::terminal(job.state()), "wait did not reach terminal state");
            const bool abnormal =
                std::string_view(mode) == "abnormal" || std::string_view(mode) == "crash";
            const int code = std::string_view(mode) == "crash" ? 139 : abnormal ? 9 : 0;
            failure(job,
                    abnormal ? jobs::FAILURE_CATEGORY_INTERNAL : jobs::FAILURE_CATEGORY_JOB_MANAGER,
                    abnormal ? "Solver/launcher exited abnormally"
                             : "Missing successful KernelExit",
                    code,
                    code);
            persisted(manager, job);
            f.captured(job);
        });
    }
    for (const bool non_executable : {false, true}) {
        suite.test(non_executable ? "non-executable is synchronous LAUNCH failure"
                                  : "missing executable is synchronous LAUNCH failure",
                   [&] {
                       LaunchFixture f;
                       const auto program = f.temp.path / "missing or non-executable solver";
                       if (non_executable)
                           write(program, "#!/bin/sh\nexit 0\n");
                       f.config.solver = program.string();
                       LocalJobManager manager(f.config);
                       const auto launched = f.submit(manager);
                       require(launched.state() == jobs::JOB_STATE_FAILED,
                               "start error was not synchronous");
                       const auto job = manager.wait(launched.run_id(), 10s);
                       require(detail::terminal(job.state()), "launch failure is nonterminal");
                       failure(job,
                               jobs::FAILURE_CATEGORY_LAUNCH,
                               "failed to start " + program.string() + ": ",
                               -1,
                               0,
                               true);
                       persisted(manager, job);
                       int status = 0;
                       errno = 0;
                       const auto result = waitpid(-1, &status, WNOHANG);
                       require(result == 0 || (result == -1 && errno == ECHILD),
                               "failed launch left an unreaped child");
                   });
    }
    for (const auto mode : {"false", "success", "abnormal"}) {
        suite.test(std::string("MPI ") + mode, [&] {
            LaunchFixture f;
            f.mode(std::string_view(mode) == "false" ? "success" : mode);
            auto p = f.profile();
            p["launcher"] = "mpirun";
            p["ranks"] = 4;
            p["mpi_args"] = Json::parse(R"(["--bind-to","core","--host","host with spaces"])");
            f.profile(p);
            if (std::string_view(mode) == "false")
                f.config.mpi = "/bin/false";
            LocalJobManager manager(f.config);
            const auto launched = f.launch(manager);
            const auto job = manager.wait(launched.run_id(), 10s);
            require(detail::terminal(job.state()), "MPI wait did not reach terminal state");
            if (std::string_view(mode) == "success") {
                completed(job, jobs::COMPLETION_KIND_END_REACHED);
                require(!job.has_failure(), "successful MPI job has a failure record");
            } else {
                const int code = std::string_view(mode) == "false" ? 1 : 9;
                failure(job,
                        jobs::FAILURE_CATEGORY_MPI,
                        "Solver/launcher exited abnormally",
                        code,
                        code);
            }
            persisted(manager, job);
            if (std::string_view(mode) != "false")
                f.captured(job);
        });
    }
    suite.test("bare executable uses inherited PATH", [] {
        LaunchFixture f;
        f.mode("success");
        const auto bin = fs::path(f.config.solver).parent_path();
        Environment path("PATH", std::string(std::getenv("PATH")) + ":" + bin.string());
        f.config.solver = "jobmgr_local_mock_solver";
        LocalJobManager manager(f.config);
        const auto launched = f.launch(manager);
        const auto job = manager.wait(launched.run_id(), 10s);
        require(detail::terminal(job.state()), "PATH lookup wait is nonterminal");
        completed(job, jobs::COMPLETION_KIND_END_REACHED);
        persisted(manager, job);
        f.captured(job);
    });
    suite.test("multiple children are collected once by their own manager", [] {
        LaunchFixture first;
        LaunchFixture other;
        ChildCleanup first_child;
        LocalJobManager owner(first.config);
        LocalJobManager other_owner(other.config);
        const auto a = first.launch(owner);
        first_child.pid = first.child.pid;
        const auto b = first.launch(owner);
        const auto foreign = other.launch(other_owner);
        require(a.run_id() != b.run_id(), "two submissions did not create distinct runs");
        first.captured(a);
        first.captured(b);
        other.captured(foreign);
        // All three are waitable before either owner polls: waitpid(-1) would
        // steal the other manager's exit, even if it ignores unknown run IDs.
        await_zombie(first_child.pid);
        await_zombie(first.child.pid);
        await_zombie(other.child.pid);
        require(owner.poll() == 2, "owner must collect exactly its two children");
        for (const auto& launched : {a, b}) {
            const auto job = owner.Get(ref(launched));
            completed(job, jobs::COMPLETION_KIND_END_REACHED);
            persisted(owner, job);
        }
        require(owner.poll() == 0, "owner returned duplicate process exits");
        require(other_owner.Get(ref(foreign)).state() == jobs::JOB_STATE_RUNNING,
                "another manager's persisted state changed");
        require(process_state(other.child.pid) == 'Z', "another owner's child was reaped");
        require(other_owner.poll() == 1, "other owner lost its child's exit");
        const auto result = other_owner.Get(ref(foreign));
        completed(result, jobs::COMPLETION_KIND_END_REACHED);
        persisted(other_owner, result);
        require(other_owner.poll() == 0, "other owner returned a duplicate process exit");
        require(owner.poll() == 0, "collecting another owner created a duplicate exit");
    });
    return suite.result();
}
#else
int main() {
    return 77;
}
#endif
