#pragma once

#include "chipcae/jobmgr/detail/spawner.hpp"
#include "chipcae/jobs/v1/jobs.pb.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace chipcae::jobmgr {
namespace jobs = chipcae::jobs::v1;
struct LocalConfig {
    std::filesystem::path run_root;
    std::string solver = "chipcae-solve";
    std::string mpi = "mpirun";
    bool cuda_available = false;
};
class LocalJobManager {
public:
    explicit LocalJobManager(LocalConfig config);
    LocalJobManager(LocalConfig config, std::unique_ptr<detail::Spawner> spawner);
    ~LocalJobManager();
    jobs::Job Submit(const jobs::SubmitRequest& request,
                     const std::filesystem::path& case_path,
                     const std::filesystem::path& profile_path);
    jobs::Job Get(const jobs::JobRef& request) const;
    jobs::ListJobsResponse List(const jobs::ListJobsRequest& request) const;
    jobs::ControlReply Control(const jobs::ControlRequest& request);
    jobs::Job Cancel(const jobs::CancelRequest& request);
    jobs::ListOutputsResponse ListOutputs(const jobs::ListOutputsRequest& request) const;
    std::size_t poll();
};
} // namespace chipcae::jobmgr
