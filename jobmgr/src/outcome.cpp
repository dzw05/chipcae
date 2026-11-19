#include "outcome.hpp"

#include "chipcae/io/event_log.hpp"
#include "run_dir.hpp"

#include <optional>

namespace chipcae::jobmgr::detail {
namespace jobs = chipcae::jobs::v1;
bool terminal(jobs::JobState state) {
    return state == jobs::JOB_STATE_COMPLETED || state == jobs::JOB_STATE_FAILED ||
           state == jobs::JOB_STATE_CANCELLED;
}
void fail(jobs::Job& job, jobs::FailureCategory category, const std::string& message, int code) {
    job.set_state(jobs::JOB_STATE_FAILED);
    auto* failure = job.mutable_failure();
    failure->set_category(category);
    failure->set_message(message);
    failure->set_rank(-1);
    failure->set_exit_code(code);
    stamp(job.mutable_ended_at());
    persist(job);
}
void process_exit(jobs::Job& job, bool mpi, const ProcessExit& exit) {
    if (terminal(job.state()))
        return;
    job.mutable_native()->set_exit_status_raw(exit.code);
    if (exit.code != 0 || !exit.normal_exit) {
        fail(job,
             mpi ? jobs::FAILURE_CATEGORY_MPI : jobs::FAILURE_CATEGORY_INTERNAL,
             "Solver/launcher exited abnormally",
             exit.code);
        return;
    }
    std::optional<chipcae::io::KernelExit> last_exit;
    try {
        const auto file = std::filesystem::path(job.result_dir()) / "events.bin";
        if (std::filesystem::is_regular_file(file)) {
            chipcae::io::EventLogReader reader(file);
            std::uint64_t offset = 0;
            const auto events = reader.read_from(0, &offset);
            if (offset == std::filesystem::file_size(file))
                for (const auto& event : events)
                    if (const auto* kernel_exit =
                            std::get_if<chipcae::io::KernelExit>(&event.payload))
                        last_exit = *kernel_exit;
        }
    } catch (const std::exception&) {
        last_exit.reset();
    }
    if (!last_exit || last_exit->exit_code != 0) {
        fail(job, jobs::FAILURE_CATEGORY_JOB_MANAGER, "Missing successful KernelExit", exit.code);
        return;
    }
    job.set_state(jobs::JOB_STATE_COMPLETED);
    job.set_completion(last_exit->reason == "checkpoint_and_stop"
                           ? jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT
                       : last_exit->reason == "user_stop" ? jobs::COMPLETION_KIND_USER_STOP
                                                          : jobs::COMPLETION_KIND_END_REACHED);
    stamp(job.mutable_ended_at());
    persist(job);
}
} // namespace chipcae::jobmgr::detail
