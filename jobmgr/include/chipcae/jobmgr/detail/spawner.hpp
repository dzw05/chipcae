#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace chipcae::jobmgr::detail {
struct LaunchCommand {
    std::string program;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> env_overrides;
};
struct ProcessExit {
    std::string run_id;
    int code;
    bool normal_exit;
};
class Spawner {
public:
    virtual ~Spawner() = default;
    virtual std::optional<long>
    start(const std::string& run_id, const LaunchCommand& command, std::string* error) = 0;
    virtual void terminate(const std::string& run_id, std::chrono::milliseconds grace) = 0;
    virtual std::vector<ProcessExit> reap() = 0;
};
} // namespace chipcae::jobmgr::detail
