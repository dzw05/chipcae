#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace chipcae::jobmgr {

enum class JobState { queued, running, completed, failed };

struct Job {
    std::string run_id;
    JobState state{JobState::queued};
};

class JobManager {
public:
    const Job& submit(std::string run_id);
    const Job* find(std::string_view run_id) const;
    const std::vector<Job>& jobs() const noexcept { return jobs_; }

private:
    std::vector<Job> jobs_;
};

} // namespace chipcae::jobmgr
