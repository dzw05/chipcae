#include "chipcae/jobmgr/job_manager.hpp"

#include <stdexcept>
#include <utility>

namespace chipcae::jobmgr {

const Job& JobManager::submit(std::string run_id) {
    if (run_id.empty()) {
        throw std::invalid_argument("run_id must not be empty");
    }
    if (find(run_id) != nullptr) {
        throw std::invalid_argument("duplicate run_id: " + run_id);
    }
    jobs_.push_back(Job{std::move(run_id), JobState::queued});
    return jobs_.back();
}

const Job* JobManager::find(std::string_view run_id) const {
    for (const auto& job : jobs_) {
        if (job.run_id == run_id) {
            return &job;
        }
    }
    return nullptr;
}

} // namespace chipcae::jobmgr
