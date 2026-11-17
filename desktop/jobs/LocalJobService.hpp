#pragma once
#include "JobTypes.hpp"
#include "PreviewWorker.hpp"

#include <QLockFile>
#include <map>
#include <memory>
namespace chipcae::desktop {
// Owner Qt thread only. The root lock excludes other service instances, including processes.
// Control is the single append API for D12 and all other local clients.
class LocalJobService : public QObject {
public:
    explicit LocalJobService(LauncherConfig config, QObject* parent = nullptr);
    ~LocalJobService() override;
    jobs::Job Submit(const jobs::SubmitRequest& request,
                     const QString& casePath,
                     const QString& jsonProfilePath);
    jobs::Job Get(const jobs::JobRef& request) const;
    jobs::ListJobsResponse List(const jobs::ListJobsRequest& request) const;
    jobs::ControlReply Control(const jobs::ControlRequest& request);
    jobs::Job Cancel(const jobs::CancelRequest& request);
    jobs::ListOutputsResponse ListOutputs(const jobs::ListOutputsRequest& request) const;

private:
    struct Run {
        jobs::Job job;
        std::unique_ptr<PreviewWorker> worker;
        std::map<std::string, jobs::ControlReply> controls;
        uint64_t cmdSeq = 0;
        bool mpi = false;
    };
    Run& run(const std::string& id);
    void persist(const Run& run) const;
    void fail(Run& run, jobs::FailureCategory category, const std::string& message, int code = -1);
    LauncherConfig config_;
    std::unique_ptr<QLockFile> lock_;
    std::map<std::string, std::unique_ptr<Run>> runs_;
};
} // namespace chipcae::desktop
