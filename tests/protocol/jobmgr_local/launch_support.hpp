#pragma once

#include "manager_support.hpp"
#include "outcome.hpp"
#include "profile_map.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <optional>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif

namespace jobmgr_test {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

class Environment {
public:
    Environment(std::string key, const std::string& value) : key_(std::move(key)) {
        if (const auto* old = std::getenv(key_.c_str()))
            old_ = old;
        require(setenv(key_.c_str(), value.c_str(), 1) == 0, "setenv failed");
    }
    ~Environment() {
        if (old_)
            setenv(key_.c_str(), old_->c_str(), 1);
        else
            unsetenv(key_.c_str());
    }
    Environment(const Environment&) = delete;
    Environment& operator=(const Environment&) = delete;

private:
    std::string key_;
    std::optional<std::string> old_;
};

// Last-resort cleanup after assertions and manager destruction. Never substitutes
// for the ESRCH / waitpid assertions that verify production ownership and reaping.
struct ChildCleanup {
    pid_t pid = -1;
    ~ChildCleanup() {
        if (pid > 0) {
            int status = 0;
            // ECHILD means the manager already reaped it; do not signal a reused PID.
            pid_t result;
            do {
                result = waitpid(pid, &status, WNOHANG);
            } while (result < 0 && errno == EINTR);
            if (result == 0) {
                kill(pid, SIGKILL);
                do {
                    result = waitpid(pid, &status, 0);
                } while (result < 0 && errno == EINTR);
            }
        }
    }
};

struct LaunchFixture : Fixture {
    Environment sentinel{"CHIPCAE_JOBMGR_SENTINEL", "1"};
    Environment omp{"OMP_NUM_THREADS", "parent-value"};
    ChildCleanup child;
    LaunchFixture() {
        config.run_root = temp.path / "运行 runs with spaces";
        const auto solver = temp.path / "求解 solver with spaces" / "jobmgr_local_mock_solver";
        fs::create_directories(solver.parent_path());
        fs::create_symlink(JOBMGR_MOCK_SOLVER, solver);
        config.solver = solver.string();
        config.mpi = JOBMGR_FAKE_MPIRUN;
        case_path = source / "输入 case with spaces.json";
        write(case_path, read(source / "case.json"));
        profile_path = source / "profile with spaces.json";
        profile(Json::parse(R"({"schema":"chipcae.run_profile/1.0","ranks":1,
            "backend":"cpu","gpus":0,"threads_per_rank":3,"launcher":"direct"})"));
    }
    void mode(std::string_view fixture, std::string_view reason = "end_reached") {
        auto p = profile();
        p["x-fixture"] = std::string(fixture);
        p["x-reason"] = std::string(reason);
        profile(p);
    }
    jobs::Job submit(LocalJobManager& manager) {
        const auto job = Fixture::submit(manager);
        if (!job.native().native_job_id().empty())
            child.pid = static_cast<pid_t>(std::stol(job.native().native_job_id()));
        return job;
    }
    jobs::Job launch(LocalJobManager& manager) {
        const auto job = submit(manager);
        require(job.state() == jobs::JOB_STATE_RUNNING,
                "real launch must return RUNNING; got " + jobs::JobState_Name(job.state()) + ": " +
                    job.failure().message());
        persisted(manager, job);
        return job;
    }
    Json ready(const jobs::Job& job) const {
        const auto file = fs::path(job.result_dir()) / "mock-argv.json";
        const auto deadline = Clock::now() + 2s;
        while (!fs::exists(file) && Clock::now() < deadline)
            std::this_thread::sleep_for(5ms);
        require(fs::exists(file), "mock did not publish readiness/argv artifact");
        return Json::parse(read(file));
    }
    void captured(const jobs::Job& job) const {
        const auto capture = ready(job);
        const auto dir = fs::path(job.result_dir());
        const auto command = chipcae::jobmgr::detail::profile_command(
            profile(), config, dir / "inputs/case.json", dir / "run.json", dir);
        auto expected = command.arguments;
        const auto solver = std::find(expected.begin(), expected.end(), config.solver);
        if (command.program == config.mpi) {
            require(solver != expected.end(), "MPI command has no solver portion");
            expected.erase(expected.begin(), solver);
        } else {
            expected.insert(expected.begin(), config.solver);
        }
        const auto actual = capture.at("argv").as<std::vector<std::string>>();
        require(actual == expected,
                "captured argv differs element-for-element from solver command");
        require(capture.at("omp").as<std::string>() == "3", "OMP override was not applied");
        require(capture.at("sentinel").as<std::string>() == "1",
                "parent environment not inherited");
        require(job.native().native_job_id() == std::to_string(capture.at("pid").as<long>()),
                "native job ID differs from real child PID");
        require(std::string(std::getenv("OMP_NUM_THREADS")) == "parent-value",
                "launch mutated parent environment");
        require(capture.at("cwd").as<std::string>() == fs::current_path().string(),
                "launch did not inherit the working directory");
    }
};
inline void cancelled(LocalJobManager& manager, const jobs::Job& job) {
    require(job.state() == jobs::JOB_STATE_CANCELLED, "Cancel must remain CANCELLED");
    require(!job.has_failure(), "cancelled job has failure");
    require(job.completion() == jobs::COMPLETION_KIND_UNSPECIFIED, "cancelled job has completion");
    persisted(manager, job);
}
inline bool gone(pid_t pid) {
    errno = 0;
    return kill(pid, 0) == -1 && errno == ESRCH;
}
inline char process_state(pid_t pid) {
#if defined(__APPLE__)
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    struct kinfo_proc info{};
    std::size_t size = sizeof(info);
    require(sysctl(mib, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info),
            "failed to read child status with sysctl");
    // Callers only distinguish zombies; querying must leave the child waitable.
    return info.kp_proc.p_stat == SZOMB ? 'Z' : '?';
#else
    const auto stat = read(fs::path("/proc") / std::to_string(pid) / "stat");
    const auto end_name = stat.rfind(')');
    require(end_name != std::string::npos && stat.size() > end_name + 2,
            "invalid /proc child status");
    return stat[end_name + 2];
#endif
}
inline void await_zombie(pid_t pid) {
    const auto deadline = Clock::now() + 2s;
    while (process_state(pid) != 'Z' && Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    require(process_state(pid) == 'Z', "child did not exit before ownership assertion");
}
inline void
reap_until_gone(LocalJobManager& manager, pid_t pid, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    do {
        manager.poll();
        if (gone(pid))
            return;
        std::this_thread::sleep_for(50ms);
    } while (Clock::now() < deadline);
    require(gone(pid), "child was not terminated and reaped before deadline");
}
} // namespace jobmgr_test
