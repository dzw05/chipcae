#pragma once

#include "chipcae/jobmgr/detail/spawner.hpp"

#include <functional>
#include <memory>
#include <utility>

namespace jobmgr_test {
class FakeSpawner final : public chipcae::jobmgr::detail::Spawner {
public:
    using LaunchCommand = chipcae::jobmgr::detail::LaunchCommand;
    using ProcessExit = chipcae::jobmgr::detail::ProcessExit;
    struct State {
        std::optional<long> pid = 4242;
        std::string error = "fake launcher unavailable";
        std::vector<std::pair<std::string, LaunchCommand>> starts;
        std::vector<std::pair<std::string, std::chrono::milliseconds>> terminations;
        std::vector<ProcessExit> exits;
        std::size_t reap_calls = 0;
        std::function<void(const std::string&, const LaunchCommand&)> on_start;
        std::function<void(const std::string&)> on_terminate;
    };
    explicit FakeSpawner(std::shared_ptr<State> state) : state_(std::move(state)) {}
    std::optional<long>
    start(const std::string& id, const LaunchCommand& command, std::string* error) override {
        state_->starts.emplace_back(id, command);
        if (state_->on_start)
            state_->on_start(id, command);
        if (!state_->pid && error)
            *error = state_->error;
        return state_->pid;
    }
    void terminate(const std::string& id, std::chrono::milliseconds grace) override {
        state_->terminations.emplace_back(id, grace);
        if (state_->on_terminate)
            state_->on_terminate(id);
    }
    std::vector<ProcessExit> reap() override {
        ++state_->reap_calls;
        return std::exchange(state_->exits, {});
    }

private:
    std::shared_ptr<State> state_;
};
} // namespace jobmgr_test
