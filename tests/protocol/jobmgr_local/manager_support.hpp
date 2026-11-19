#pragma once

#include "chipcae/io/event_log.hpp"
#include "chipcae/jobmgr/local_job_manager.hpp"
#include "chipcae/jobmgr/local_paths.hpp"
#include "fake_spawner.hpp"
#include "test_support.hpp"

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <iomanip>
#include <jsoncons/json.hpp>
#include <sstream>

namespace jobmgr_test {
namespace jobs = chipcae::jobs::v1;
using chipcae::jobmgr::LocalConfig;
using chipcae::jobmgr::LocalJobManager;
using Json = jsoncons::json;

inline jobs::JobRef ref(const jobs::Job& job) {
    jobs::JobRef result;
    result.set_run_id(job.run_id());
    return result;
}
inline jobs::CancelRequest cancel_request(const jobs::Job& job, std::int64_t seconds = 0) {
    jobs::CancelRequest result;
    result.set_run_id(job.run_id());
    result.mutable_grace()->set_seconds(seconds);
    return result;
}
inline jobs::ListOutputsRequest outputs_request(const jobs::Job& job) {
    jobs::ListOutputsRequest result;
    result.set_run_id(job.run_id());
    result.set_since_window(-1);
    return result;
}
inline auto stamp(const google::protobuf::Timestamp& timestamp) {
    return std::pair(timestamp.seconds(), timestamp.nanos());
}
inline jobs::RunProfile
mapped_profile(unsigned ranks = 4, unsigned threads = 2, unsigned gpus = 0) {
    jobs::RunProfile profile;
    profile.set_scheduler(jobs::SCHEDULER_TYPE_LOCAL);
    profile.set_nodes(1);
    profile.set_ranks(ranks);
    profile.set_ranks_per_node(ranks);
    profile.set_cpus_per_rank(threads);
    profile.set_gpus_per_node(gpus);
    (*profile.mutable_env())["OMP_NUM_THREADS"] = std::to_string(threads);
    return profile;
}
template <class Message>
Message parse_pb(const fs::path& path) {
    Message message;
    require(message.ParseFromString(read(path)), "invalid protobuf at " + path.string());
    return message;
}
// Reconstructed Jobs contain maps. Compare all fields using a deterministic
// encoding, while tests of unchanged on-disk files still compare raw bytes.
inline std::string serialize_job(const jobs::Job& job) {
    std::string bytes;
    {
        google::protobuf::io::StringOutputStream sink(&bytes);
        google::protobuf::io::CodedOutputStream output(&sink);
        output.SetSerializationDeterministic(true);
        require(job.SerializeToCodedStream(&output) && !output.HadError(),
                "Job serialization failed");
    }
    return bytes;
}
inline void persisted(const LocalJobManager& manager, const jobs::Job& job) {
    const auto disk = parse_pb<jobs::Job>(fs::path(job.result_dir()) / "job.pb");
    require(serialize_job(disk) == serialize_job(manager.Get(ref(job))),
            "persisted Job differs from Get");
    require(serialize_job(disk) == serialize_job(job), "returned Job differs from disk");
}
inline void failure(const jobs::Job& job,
                    jobs::FailureCategory category,
                    std::string_view message,
                    int code = -1,
                    int raw = 0,
                    bool prefix = false) {
    require(job.state() == jobs::JOB_STATE_FAILED,
            "expected FAILED, got " + jobs::JobState_Name(job.state()));
    require(job.failure().category() == category, "wrong failure category");
    require(prefix ? job.failure().message().starts_with(message)
                   : job.failure().message() == message,
            "wrong failure message: " + job.failure().message());
    require(job.failure().rank() == -1, "failure rank must be -1");
    require(job.failure().exit_code() == code, "wrong failure exit_code");
    require(job.native().exit_status_raw() == raw, "wrong native raw status");
    require(job.completion() == jobs::COMPLETION_KIND_UNSPECIFIED, "failed job has completion");
    require(job.ended_at().seconds() > 0, "failure missing ended_at");
    require(stamp(job.ended_at()) >= stamp(job.submitted_at()), "ended_at precedes submitted_at");
    if (job.has_started_at())
        require(stamp(job.ended_at()) >= stamp(job.started_at()), "ended_at precedes started_at");
}
inline void completed(const jobs::Job& job, jobs::CompletionKind kind) {
    require(job.state() == jobs::JOB_STATE_COMPLETED,
            "expected COMPLETED, got " + jobs::JobState_Name(job.state()));
    require(job.completion() == kind, "wrong completion kind");
    require(job.failure().category() == jobs::FAILURE_CATEGORY_UNSPECIFIED,
            "successful job has failure category");
    require(job.failure().message().empty(), "successful job has failure message");
    require(job.failure().rank() == 0 && job.failure().exit_code() == 0,
            "successful job has failure rank/code");
    require(job.native().exit_status_raw() == 0, "success raw status");
    require(job.ended_at().seconds() > 0, "completion missing ended_at");
    require(stamp(job.ended_at()) >= stamp(job.started_at()), "nonmonotonic completion timestamp");
}
inline void events(const jobs::Job& job, const std::vector<chipcae::io::EventPayload>& payloads) {
    chipcae::io::EventLogWriter writer(fs::path(job.result_dir()) / "events.bin");
    std::uint64_t seq = 0;
    for (const auto& payload : payloads)
        writer.append({++seq, 1, payload});
    writer.close();
}
inline std::size_t directory_count(const fs::path& root) {
    std::size_t count = 0;
    if (fs::exists(root))
        for (const auto& entry : fs::directory_iterator(root))
            if (entry.is_directory())
                ++count;
    return count;
}
struct Fixture {
    TempDirectory temp;
    fs::path source = temp.path / "source";
    fs::path case_path = source / "case.json";
    fs::path profile_path = source / "profile.json";
    LocalConfig config{temp.path / "runs", "solver-test", "mpi-test", false};
    std::shared_ptr<FakeSpawner::State> fake = std::make_shared<FakeSpawner::State>();
    unsigned request_number = 0;

    Fixture() {
        const fs::path examples = fs::path(CHIPCAE_SOURCE_DIR) / "schema/examples";
        for (const auto name : {"fcbga_demo.stack.json", "fcbga_demo.materials.json"})
            write(source / name, read(examples / name));
        write(case_path, read(examples / "fcbga_demo.case.json"));
        write(profile_path, read(examples / "local.run-profile.json"));
    }
    std::string id() {
        std::ostringstream out;
        out << "00000000-0000-4000-8000-" << std::setw(12) << std::setfill('0') << ++request_number;
        return out.str();
    }
    jobs::SubmitRequest request() {
        jobs::SubmitRequest result;
        result.set_client_request_id(id());
        result.set_name("synthetic FCBGA");
        (*result.mutable_labels())["project"] = "demo";
        return result;
    }
    jobs::ControlRequest control(const jobs::Job& job,
                                 jobs::ControlAction action = jobs::CONTROL_ACTION_PAUSE) {
        jobs::ControlRequest result;
        result.set_run_id(job.run_id());
        result.set_client_request_id(id());
        result.set_action(action);
        return result;
    }
    std::unique_ptr<LocalJobManager> manager() {
        return std::make_unique<LocalJobManager>(config, std::make_unique<FakeSpawner>(fake));
    }
    jobs::Job submit(LocalJobManager& manager) {
        return manager.Submit(request(), case_path, profile_path);
    }
    jobs::Job running(LocalJobManager& manager) {
        auto job = submit(manager);
        require(job.state() == jobs::JOB_STATE_RUNNING,
                "valid Submit must be RUNNING; got " + jobs::JobState_Name(job.state()));
        persisted(manager, job);
        return job;
    }
    void finish(const jobs::Job& job, int code = 0, bool normal = true) {
        fake->exits.push_back({job.run_id(), code, normal});
    }
    Json profile() const { return Json::parse(read(profile_path)); }
    Json case_json() const { return Json::parse(read(case_path)); }
    void profile(const Json& value) { write(profile_path, value.to_string()); }
    void case_json(const Json& value) { write(case_path, value.to_string()); }
};
} // namespace jobmgr_test
