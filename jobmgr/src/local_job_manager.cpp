#include "chipcae/jobmgr/local_job_manager.hpp"

#include "chipcae/jobmgr/local_paths.hpp"
#include "launcher_lock.hpp"
#include "outcome.hpp"
#include "profile_map.hpp"
#include "run_dir.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace chipcae::jobmgr {
namespace {
class UnavailableSpawner final : public detail::Spawner {
public:
    std::optional<long>
    start(const std::string&, const detail::LaunchCommand&, std::string* error) override {
        if (error)
            *error = "process launcher not available in this build";
        return std::nullopt;
    }
    void terminate(const std::string&, std::chrono::milliseconds) override {}
    std::vector<detail::ProcessExit> reap() override { return {}; }
};
void uuid(const std::string& id) {
    if (!is_uuid(id))
        throw std::runtime_error("client_request_id must be a UUID");
}
std::chrono::milliseconds grace_period(const google::protobuf::Duration& grace) {
    // Bound seconds before multiplication, including noncanonical input at int64 extremes.
    if (grace.seconds() < -2)
        return std::chrono::milliseconds(0);
    if (grace.seconds() > 62)
        return std::chrono::milliseconds(60000);
    const auto milliseconds = grace.seconds() * 1000 + grace.nanos() / 1000000;
    return std::chrono::milliseconds(std::clamp<std::int64_t>(milliseconds, 0, 60000));
}
} // namespace
class LocalJobManager::Impl {
public:
    struct Run {
        jobs::Job job;
        std::map<std::string, jobs::ControlReply> controls;
        std::uint64_t sequence = 0;
        bool mpi = false;
        bool owned = false;
    };
    LocalConfig config;
    std::unique_ptr<detail::LauncherLock> lock;
    std::unique_ptr<detail::Spawner> spawner;
    std::map<std::string, Run> runs;

    Impl(LocalConfig c, std::unique_ptr<detail::Spawner> s)
        : config(std::move(c)), spawner(std::move(s)) {
        config.run_root = std::filesystem::absolute(config.run_root).lexically_normal();
        lock = std::make_unique<detail::LauncherLock>(config.run_root);
        if (!spawner)
            spawner = std::make_unique<UnavailableSpawner>();
        for (const auto& entry : std::filesystem::directory_iterator(config.run_root)) {
            if (!entry.is_directory() || !std::filesystem::exists(entry.path() / "job.pb"))
                continue;
            Run run;
            if (!run.job.ParseFromString(detail::read_file(entry.path() / "job.pb")))
                throw std::runtime_error("Invalid persisted Job");
            const auto journal = entry.path() / "control.jsonl";
            if (std::filesystem::exists(journal)) {
                const auto bytes = detail::read_file(journal);
                if (!bytes.empty() && bytes.back() != '\n')
                    throw std::runtime_error(
                        "Incomplete broker journal; repair required before append");
                for (std::size_t offset = 0; offset < bytes.size();) {
                    const auto end = bytes.find('\n', offset);
                    const auto command = detail::Json::parse(bytes.substr(offset, end - offset));
                    std::uint64_t sequence = 0;
                    if (!command.is_object() || !command.contains("cmd_seq") ||
                        !command.at("cmd_seq").is_string())
                        throw std::runtime_error("Invalid broker journal sequence");
                    const auto text = command.at("cmd_seq").as<std::string>();
                    const auto parsed =
                        std::from_chars(text.data(), text.data() + text.size(), sequence);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
                        sequence <= run.sequence)
                        throw std::runtime_error("Invalid broker journal sequence");
                    run.sequence = sequence;
                    jobs::ControlReply reply;
                    reply.set_cmd_seq(sequence);
                    reply.set_delivery(jobs::CONTROL_DELIVERY_QUEUED);
                    run.controls.emplace(command.at("client_request_id").as<std::string>(), reply);
                    offset = end + 1;
                }
            }
            if (!detail::terminal(run.job.state()))
                detail::fail(run.job,
                             jobs::FAILURE_CATEGORY_JOB_MANAGER,
                             "Launcher restarted without process ownership");
            const auto id = run.job.run_id();
            if (!runs.emplace(id, std::move(run)).second)
                throw std::runtime_error("Invalid persisted Job");
        }
    }
    ~Impl() {
        // Terminate before destroying the spawner, run storage, or root lock.
        for (const auto& [id, run] : runs)
            if (run.owned) {
                try {
                    spawner->terminate(id, std::chrono::milliseconds(0));
                } catch (...) {
                    // Destruction must still release the root lock if a launcher fails.
                }
            }
    }
    Run& run(const std::string& id) {
        auto it = runs.find(id);
        if (it == runs.end())
            throw std::runtime_error("Unknown run_id");
        return it->second;
    }
    const Run& run(const std::string& id) const {
        auto it = runs.find(id);
        if (it == runs.end())
            throw std::runtime_error("Unknown run_id");
        return it->second;
    }
};
LocalJobManager::LocalJobManager(LocalConfig config)
    : LocalJobManager(std::move(config), std::make_unique<UnavailableSpawner>()) {}
LocalJobManager::LocalJobManager(LocalConfig config, std::unique_ptr<detail::Spawner> spawner)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(spawner))) {}
LocalJobManager::~LocalJobManager() = default;
// TEST-FIRST compilation stub: polling and timeout behavior remain unimplemented.
jobs::Job LocalJobManager::wait(const std::string& run_id, std::chrono::milliseconds) {
    jobs::JobRef request;
    request.set_run_id(run_id);
    return Get(request);
}
jobs::Job LocalJobManager::Submit(const jobs::SubmitRequest& request,
                                  const std::filesystem::path& case_path,
                                  const std::filesystem::path& profile_path) {
    uuid(request.client_request_id());
    for (const auto& [id, run] : impl_->runs)
        if (run.job.client_request_id() == request.client_request_id())
            return run.job;
    Impl::Run run;
    auto& job = run.job;
    std::filesystem::path directory;
    do {
        job.set_run_id(make_run_id());
        directory = impl_->config.run_root / job.run_id();
    } while (!std::filesystem::create_directory(directory));
    job.set_client_request_id(request.client_request_id());
    job.set_name(request.name());
    const auto* user = std::getenv("USER");
    job.set_owner(user ? user : "");
    job.set_attempt(1);
    job.set_result_dir(directory.string());
    job.set_result_dir_local(true);
    job.set_state(jobs::JOB_STATE_SUBMITTING);
    *job.mutable_labels() = request.labels();
    (*job.mutable_labels())["desktop.solver"] = impl_->config.solver;
    detail::stamp(job.mutable_submitted_at());
    write_file_atomic(directory / "control.jsonl", {});
    detail::persist(job);
    const auto id = job.run_id();
    auto& active = impl_->runs.emplace(id, std::move(run)).first->second;
    auto& j = active.job;
    detail::LaunchCommand command;
    try {
        detail::Json hashes(jsoncons::json_object_arg);
        std::set<std::filesystem::path> visited;
        detail::snapshot(case_path,
                         directory / "inputs/case.json",
                         hashes,
                         visited,
                         request.case_file().sha256());
        detail::snapshot(profile_path, directory / "run.json", hashes, visited);
        for (const auto& input : request.inputs()) {
            const auto& name = input.logical_name();
            try {
                if (name.empty())
                    throw std::runtime_error("Unsafe input logical_name");
                check_snapshot_ref(name);
            } catch (const std::exception&) {
                throw std::runtime_error("Unsafe input logical_name");
            }
            detail::snapshot(case_path.parent_path() / name,
                             directory / "inputs" / name,
                             hashes,
                             visited,
                             input.sha256());
        }
        detail::validate_case(directory / "inputs/case.json");
        j.set_case_sha256(sha256_hex(detail::read_file(directory / "inputs/case.json")));
        const auto profile = detail::read_object(directory / "run.json");
        const auto mapped = detail::map_profile(profile, impl_->config.cuda_available);
        if (request.has_profile() &&
            request.profile().SerializeAsString() != mapped.SerializeAsString())
            throw std::runtime_error("RunProfile disagrees with JSON profile");
        *j.mutable_profile() = mapped;
        j.mutable_native()->set_scheduler(jobs::SCHEDULER_TYPE_LOCAL);
        write_file_atomic(directory / "snapshot-hashes.json", hashes.to_string());
        write_file_atomic(directory / "submit.pb", request.SerializeAsString());
        command = detail::profile_command(profile,
                                          impl_->config,
                                          directory / "inputs/case.json",
                                          directory / "run.json",
                                          directory);
        active.mpi = command.program == impl_->config.mpi;
    } catch (const std::exception& error) {
        detail::fail(j, jobs::FAILURE_CATEGORY_CASE_INVALID, error.what());
        return j;
    }
    j.set_state(jobs::JOB_STATE_STARTING);
    detail::stamp(j.mutable_started_at());
    detail::persist(j);
    std::string error;
    const auto pid = impl_->spawner->start(id, command, &error);
    if (!pid) {
        detail::fail(j, jobs::FAILURE_CATEGORY_LAUNCH, error);
    } else {
        active.owned = true;
        j.set_state(jobs::JOB_STATE_RUNNING);
        j.mutable_native()->set_native_job_id(std::to_string(*pid));
        detail::stamp(j.mutable_started_at());
        detail::persist(j);
    }
    return j;
}
jobs::Job LocalJobManager::Get(const jobs::JobRef& request) const {
    return std::as_const(*impl_).run(request.run_id()).job;
}
jobs::ListJobsResponse LocalJobManager::List(const jobs::ListJobsRequest& request) const {
    const auto& selector = request.label_selector();
    const auto equals = selector.find('=');
    if (!selector.empty() &&
        (equals == std::string::npos || selector.find('=', equals + 1) != std::string::npos))
        throw std::runtime_error("label_selector must be key=value");
    jobs::ListJobsResponse result;
    for (const auto& [id, run] : impl_->runs) {
        if (id <= request.page_token())
            continue;
        if (!request.states().empty() &&
            std::find(request.states().begin(), request.states().end(), run.job.state()) ==
                request.states().end())
            continue;
        if (!selector.empty()) {
            const auto label = run.job.labels().find(selector.substr(0, equals));
            if (label == run.job.labels().end() || label->second != selector.substr(equals + 1))
                continue;
        }
        if (request.page_size() &&
            static_cast<std::uint32_t>(result.jobs_size()) == request.page_size()) {
            result.set_next_page_token(result.jobs(result.jobs_size() - 1).run_id());
            break;
        }
        *result.add_jobs() = run.job;
    }
    return result;
}
jobs::ControlReply LocalJobManager::Control(const jobs::ControlRequest& request) {
    poll();
    uuid(request.client_request_id());
    auto& run = impl_->run(request.run_id());
    if (auto cached = run.controls.find(request.client_request_id()); cached != run.controls.end())
        return cached->second;
    jobs::ControlReply reply;
    reply.set_delivery(jobs::CONTROL_DELIVERY_REJECTED);
    const bool interval = request.action() == jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL;
    if (detail::terminal(run.job.state()) || request.action() < jobs::CONTROL_ACTION_PAUSE ||
        request.action() > jobs::CONTROL_ACTION_CHECKPOINT_AND_STOP ||
        (interval && request.arg_case() == jobs::ControlRequest::ARG_NOT_SET) ||
        (!interval && request.arg_case() != jobs::ControlRequest::ARG_NOT_SET) ||
        (request.has_output_interval_time() &&
         (!std::isfinite(request.output_interval_time()) || request.output_interval_time() <= 0)) ||
        (request.has_output_interval_steps() && request.output_interval_steps() == 0)) {
        reply.set_message("State/action/interval does not permit control");
        return reply;
    }
    if (run.sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Invalid broker journal sequence");
    const auto sequence = run.sequence + 1;
    detail::Json command(jsoncons::json_object_arg);
    command["issuedAt"] = detail::issued_at();
    command["cmd_seq"] = std::to_string(sequence);
    command["client_request_id"] = request.client_request_id();
    command["action"] = jobs::ControlAction_Name(request.action())
                            .substr(std::string_view("CONTROL_ACTION_").size());
    if (request.has_output_interval_time())
        command["output_interval_time"] = request.output_interval_time();
    if (request.has_output_interval_steps())
        command["output_interval_steps"] = std::to_string(request.output_interval_steps());
    detail::append_control(std::filesystem::path(run.job.result_dir()) / "control.jsonl",
                           command.to_string() + '\n');
    run.sequence = sequence;
    reply.set_cmd_seq(sequence);
    reply.set_delivery(jobs::CONTROL_DELIVERY_QUEUED);
    run.controls.emplace(request.client_request_id(), reply);
    return reply;
}
jobs::Job LocalJobManager::Cancel(const jobs::CancelRequest& request) {
    poll();
    auto& run = impl_->run(request.run_id());
    if (!detail::terminal(run.job.state())) {
        run.job.set_state(jobs::JOB_STATE_CANCELLED);
        detail::stamp(run.job.mutable_ended_at());
        detail::persist(run.job);
        if (run.owned)
            impl_->spawner->terminate(request.run_id(), grace_period(request.grace()));
    }
    return run.job;
}
jobs::ListOutputsResponse
LocalJobManager::ListOutputs(const jobs::ListOutputsRequest& request) const {
    const auto& job = std::as_const(*impl_).run(request.run_id()).job;
    jobs::ListOutputsResponse result;
    const auto file = std::filesystem::path(job.result_dir()) / "manifest.json";
    if (!std::filesystem::exists(file))
        return result;
    const auto manifest = detail::read_object(file);
    result.set_manifest_path("manifest.json");
    result.set_manifest_generation(manifest.get_value_or<std::uint64_t>("generation", 0));
    if (request.since_generation() && result.manifest_generation() <= request.since_generation())
        return result;
    if (!manifest.contains("steps"))
        return result;
    for (const auto& entry : manifest.at("steps").array_range()) {
        const auto window = entry.get_value_or<std::int64_t>("n", 0);
        if (window <= request.since_window())
            continue;
        auto* step = result.add_steps();
        step->set_window(window);
        step->set_time(entry.get_value_or<double>("time", 0));
        step->set_complete(entry.get_value_or<bool>("complete", true));
        auto* output = step->add_files();
        output->set_path(entry.get_value_or<std::string>("file", ""));
        output->set_sha256(entry.get_value_or<std::string>("checksum", ""));
        if (entry.contains("blocks"))
            for (const auto& block : entry.at("blocks").array_range())
                step->add_blocks(block.as<std::string>());
    }
    return result;
}
std::size_t LocalJobManager::poll() {
    const auto exits = impl_->spawner->reap();
    for (const auto& exit : exits) {
        const auto found = impl_->runs.find(exit.run_id);
        if (found == impl_->runs.end())
            continue;
        auto& run = found->second;
        run.owned = false;
        detail::process_exit(run.job, run.mpi, exit);
    }
    return exits.size();
}
} // namespace chipcae::jobmgr
