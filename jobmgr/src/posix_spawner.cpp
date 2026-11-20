#include "posix_spawner.hpp"

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <map>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace chipcae::jobmgr::detail {
namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// Normal reaping, failed publication, and shutdown all collect only an owned PID.
pid_t collect(pid_t pid, int& status, int options = WNOHANG) {
    pid_t result;
    do {
        result = waitpid(pid, &status, options);
    } while (result == -1 && errno == EINTR);
    return result;
}
} // namespace

class PosixSpawner::Impl {
public:
    struct Child {
        pid_t pid = -1;
        bool reaped = false;
        bool stopping = false;
        std::condition_variable wake;
        std::thread watchdog;
    };
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<Child>> children;

    ~Impl() {
        const auto deadline = Clock::now() + 3s;
        {
            std::lock_guard guard(mutex);
            for (const auto& [id, child] : children) {
                child->stopping = true;
                child->wake.notify_all();
                kill(child->pid, SIGKILL);
            }
        }
        // No caller can access this owner during destruction. Wake before joining,
        // and never join under the mutex the watchdog needs in order to exit.
        for (const auto& [id, child] : children)
            if (child->watchdog.joinable())
                child->watchdog.join();
        while (!children.empty()) {
            for (auto it = children.begin(); it != children.end();) {
                int status = 0;
                const auto result = collect(it->second->pid, status);
                if (result > 0 || (result == -1 && errno == ECHILD))
                    it = children.erase(it);
                else
                    ++it;
            }
            const auto now = Clock::now();
            if (children.empty() || now >= deadline)
                break;
            std::this_thread::sleep_for(std::min(Clock::duration(10ms), deadline - now));
        }
        // A child stuck in uninterruptible kernel sleep can outlive SIGKILL and
        // this shared deadline. Keep shutdown bounded; do not block in waitpid.
    }
};

PosixSpawner::PosixSpawner() : impl_(std::make_unique<Impl>()) {}
PosixSpawner::~PosixSpawner() = default;

std::optional<long>
PosixSpawner::start(const std::string& run_id, const LaunchCommand& command, std::string* error) {
    std::vector<char*> argv;
    argv.reserve(command.arguments.size() + 2);
    argv.push_back(const_cast<char*>(command.program.c_str()));
    for (const auto& argument : command.arguments)
        argv.push_back(const_cast<char*>(argument.c_str()));
    argv.push_back(nullptr);

    std::map<std::string, std::string> environment;
    for (auto entry = environ; entry && *entry; ++entry) {
        const std::string value(*entry);
        const auto equals = value.find('=');
        if (equals != std::string::npos)
            environment[value.substr(0, equals)] = value.substr(equals + 1);
    }
    for (const auto& [key, value] : command.env_overrides)
        environment[key] = value;
    std::vector<std::string> storage;
    storage.reserve(environment.size());
    for (const auto& [key, value] : environment)
        storage.push_back(key + '=' + value);
    std::vector<char*> envp;
    envp.reserve(storage.size() + 1);
    for (auto& value : storage)
        envp.push_back(value.data());
    envp.push_back(nullptr);

    auto child = std::make_unique<Impl::Child>();
    std::lock_guard guard(impl_->mutex);
    if (impl_->children.contains(run_id))
        throw std::runtime_error("Process already owned for run_id");
    pid_t pid = -1;
    const auto result =
        posix_spawnp(&pid, command.program.c_str(), nullptr, nullptr, argv.data(), envp.data());
    if (result != 0) {
        if (error)
            *error = "failed to start " + command.program + ": " + std::strerror(result);
        return std::nullopt;
    }
    child->pid = pid;
    try {
        impl_->children.emplace(run_id, std::move(child));
    } catch (...) {
        kill(pid, SIGKILL);
        int status = 0;
        collect(pid, status, 0);
        throw;
    }
    return static_cast<long>(pid);
}

void PosixSpawner::terminate(const std::string& run_id, std::chrono::milliseconds grace) {
    std::lock_guard guard(impl_->mutex);
    const auto found = impl_->children.find(run_id);
    if (found == impl_->children.end())
        return;
    auto& child = *found->second;
    if (child.stopping || child.watchdog.joinable())
        return;
    kill(child.pid, SIGTERM);
    if (grace <= 0ms) {
        child.stopping = true;
        kill(child.pid, SIGKILL);
        return;
    }
    const auto deadline = Clock::now() + grace;
    try {
        child.watchdog = std::thread([owner = impl_.get(), record = &child, deadline] {
            std::unique_lock lock(owner->mutex);
            if (!record->wake.wait_until(
                    lock, deadline, [&] { return record->reaped || record->stopping; }))
                kill(record->pid, SIGKILL);
        });
    } catch (...) {
        // Thread creation failure must not strand a TERM-ignoring child.
        // Kill and collect synchronously before abandoning ownership.
        child.stopping = true;
        kill(child.pid, SIGKILL);
        int status = 0;
        collect(child.pid, status, 0);
        impl_->children.erase(found);
        throw;
    }
}

std::vector<ProcessExit> PosixSpawner::reap() {
    std::vector<ProcessExit> exits;
    std::vector<std::unique_ptr<Impl::Child>> collected;
    {
        std::lock_guard guard(impl_->mutex);
        exits.reserve(impl_->children.size());
        collected.reserve(impl_->children.size());
        std::vector<ProcessExit> candidates;
        candidates.reserve(impl_->children.size());
        for (const auto& [id, child] : impl_->children)
            candidates.push_back({id, 0, false});
        std::size_t index = 0;
        for (auto it = impl_->children.begin(); it != impl_->children.end();) {
            auto& child = *it->second;
            auto& exit = candidates[index++]; // All allocations precede consuming any exits.
            int status = 0;
            const auto result = collect(child.pid, status);
            if (result == 0 || (result == -1 && errno != ECHILD)) {
                ++it;
                continue;
            }
            if (result > 0) {
                exit.normal_exit = WIFEXITED(status);
                exit.code = exit.normal_exit ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
                exits.push_back(std::move(exit));
            }
            child.reaped = true;
            child.wake.notify_all();
            collected.push_back(std::move(it->second));
            it = impl_->children.erase(it);
        }
    }
    for (const auto& child : collected)
        if (child->watchdog.joinable())
            child->watchdog.join();
    return exits;
}
} // namespace chipcae::jobmgr::detail
