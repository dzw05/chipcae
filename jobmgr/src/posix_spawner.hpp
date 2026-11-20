#pragma once

#include "chipcae/jobmgr/detail/spawner.hpp"

#include <memory>

namespace chipcae::jobmgr::detail {
class PosixSpawner final : public Spawner {
public:
    PosixSpawner();
    ~PosixSpawner() override;
    std::optional<long>
    start(const std::string& run_id, const LaunchCommand& command, std::string* error) override;
    void terminate(const std::string& run_id, std::chrono::milliseconds grace) override;
    std::vector<ProcessExit> reap() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chipcae::jobmgr::detail
