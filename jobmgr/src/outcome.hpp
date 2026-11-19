#pragma once

#include "chipcae/jobmgr/detail/spawner.hpp"
#include "chipcae/jobs/v1/jobs.pb.h"

namespace chipcae::jobmgr::detail {
bool terminal(chipcae::jobs::v1::JobState state);
void fail(chipcae::jobs::v1::Job& job,
          chipcae::jobs::v1::FailureCategory category,
          const std::string& message,
          int code = -1);
void process_exit(chipcae::jobs::v1::Job& job, bool mpi, const ProcessExit& exit);
} // namespace chipcae::jobmgr::detail
