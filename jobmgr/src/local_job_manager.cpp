#include "chipcae/jobmgr/local_job_manager.hpp"

// TEST-FIRST linkage stubs only: no filesystem, process or state-machine behavior.
namespace chipcae::jobmgr {
LocalJobManager::LocalJobManager(LocalConfig) {}
LocalJobManager::LocalJobManager(LocalConfig, std::unique_ptr<detail::Spawner>) {}
LocalJobManager::~LocalJobManager() = default;
jobs::Job LocalJobManager::Submit(const jobs::SubmitRequest&,
                                  const std::filesystem::path&,
                                  const std::filesystem::path&) {
    return {};
}
jobs::Job LocalJobManager::Get(const jobs::JobRef&) const {
    return {};
}
jobs::ListJobsResponse LocalJobManager::List(const jobs::ListJobsRequest&) const {
    return {};
}
jobs::ControlReply LocalJobManager::Control(const jobs::ControlRequest&) {
    return {};
}
jobs::Job LocalJobManager::Cancel(const jobs::CancelRequest&) {
    return {};
}
jobs::ListOutputsResponse LocalJobManager::ListOutputs(const jobs::ListOutputsRequest&) const {
    return {};
}
std::size_t LocalJobManager::poll() {
    return 0;
}
} // namespace chipcae::jobmgr
