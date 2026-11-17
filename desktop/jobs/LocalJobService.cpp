#include "LocalJobService.hpp"

#include "CaseDocument.hpp"
#include "ProfileMapper.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUuid>
#include <cmath>
#include <set>
#include <stdexcept>
namespace chipcae::desktop {
namespace {
void stamp(google::protobuf::Timestamp* t) {
    t->set_seconds(QDateTime::currentSecsSinceEpoch());
}
void uuid(const std::string& id) {
    if (QUuid(QString::fromStdString(id)).isNull())
        throw std::runtime_error("client_request_id must be a UUID");
}
QJsonObject object(const QByteArray& bytes) {
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        throw std::runtime_error("Invalid JSON object");
    return doc.object();
}
QString path(const jobs::Job& job, const QString& file) {
    return QDir(QString::fromStdString(job.result_dir())).filePath(file);
}
// Snapshot exact bytes and preserve relative reference relationships. No live input is used later.
void snapshot(const QString& source,
              const QString& target,
              QJsonObject& hashes,
              std::set<QString>& visited,
              const QString& expected = {}) {
    auto raw = readJobFile(source);
    auto hash = sha256(raw);
    if (!expected.isEmpty() && hash.compare(expected, Qt::CaseInsensitive) != 0)
        throw std::runtime_error("INPUT_HASH_MISMATCH");
    if (!visited.insert(target).second)
        return;
    QDir().mkpath(QFileInfo(target).absolutePath());
    writeJobFile(target, raw);
    hashes[target] = hash;
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError)
        return; // Binary polysets are inputs too.
    std::function<void(const QJsonValue&)> visit = [&](const QJsonValue& value) {
        if (value.isArray()) {
            for (auto child : value.toArray())
                visit(child);
        } else if (value.isObject()) {
            auto o = value.toObject();
            const bool officialRef = o.contains("ref");
            const bool hashedPath = o.contains("path") && o.contains("sha256");
            if (officialRef || hashedPath) {
                auto ref = o.value(officialRef ? "ref" : "path").toString();
                if (!ref.isEmpty()) {
                    if (QDir::isAbsolutePath(ref) || ref.split('/').contains("..") ||
                        ref.contains('\\') || ref.contains(':'))
                        throw std::runtime_error("Unsafe snapshot reference");
                    snapshot(QFileInfo(source).dir().filePath(ref),
                             QFileInfo(target).dir().filePath(ref),
                             hashes,
                             visited,
                             o.value("sha256").toString());
                }
            }
            for (auto it = o.begin(); it != o.end(); ++it) {
                if (it.key().startsWith("x-"))
                    continue;
                visit(it.value());
            }
        }
    };
    visit(doc.object());
}
} // namespace
LocalJobService::LocalJobService(LauncherConfig config, QObject* parent)
    : QObject(parent), config_(std::move(config)) {
    config_.runRoot = QFileInfo(config_.runRoot).absoluteFilePath();
    QDir().mkpath(config_.runRoot);
    lock_ = std::make_unique<QLockFile>(QDir(config_.runRoot).filePath("launcher.lock"));
    if (!lock_->tryLock())
        throw std::runtime_error("Local job root already has a control writer");
    for (auto dir : QDir(config_.runRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString file = QDir(config_.runRoot).filePath(dir + "/job.pb");
        if (!QFileInfo::exists(file))
            continue;
        auto r = std::make_unique<Run>();
        if (!r->job.ParseFromString(readJobFile(file).toStdString()))
            throw std::runtime_error("Invalid persisted Job");
        QFile controls(path(r->job, "control.jsonl"));
        if (controls.open(QIODevice::ReadOnly)) {
            while (!controls.atEnd()) {
                auto line = controls.readLine();
                if (!line.endsWith('\n'))
                    throw std::runtime_error(
                        "Incomplete broker journal; repair required before append");
                auto c = object(line);
                bool ok = false;
                auto seq = c.value("cmd_seq").toString().toULongLong(&ok);
                if (!ok || seq <= r->cmdSeq)
                    throw std::runtime_error("Invalid broker journal sequence");
                r->cmdSeq = seq;
                jobs::ControlReply reply;
                reply.set_cmd_seq(seq);
                reply.set_delivery(jobs::CONTROL_DELIVERY_QUEUED);
                r->controls.emplace(c.value("client_request_id").toString().toStdString(), reply);
            }
        }
        if (!terminal(r->job.state()))
            fail(*r,
                 jobs::FAILURE_CATEGORY_JOB_MANAGER,
                 "Launcher restarted without process ownership");
        runs_.emplace(r->job.run_id(), std::move(r));
    }
}
LocalJobService::~LocalJobService() {
    // Kill owned children while callbacks and run storage are still alive.
    for (auto& [id, r] : runs_) {
        if (r->worker) {
            r->worker->disconnect(this);
            r->worker.reset();
        }
    }
}
LocalJobService::Run& LocalJobService::run(const std::string& id) {
    auto it = runs_.find(id);
    if (it == runs_.end())
        throw std::runtime_error("Unknown run_id");
    return *it->second;
}
void LocalJobService::persist(const Run& r) const {
    writeJobFile(path(r.job, "job.pb"), QByteArray::fromStdString(r.job.SerializeAsString()));
}
void LocalJobService::fail(Run& r,
                           jobs::FailureCategory category,
                           const std::string& message,
                           int code) {
    r.job.set_state(jobs::JOB_STATE_FAILED);
    auto* f = r.job.mutable_failure();
    f->set_category(category);
    f->set_message(message);
    f->set_rank(-1);
    f->set_exit_code(code);
    stamp(r.job.mutable_ended_at());
    persist(r);
}
jobs::Job LocalJobService::Submit(const jobs::SubmitRequest& request,
                                  const QString& casePath,
                                  const QString& profilePath) {
    uuid(request.client_request_id());
    for (auto& [id, r] : runs_)
        if (r->job.client_request_id() == request.client_request_id())
            return r->job;
    auto r = std::make_unique<Run>();
    auto& j = r->job;
    j.set_run_id(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString());
    j.set_client_request_id(request.client_request_id());
    j.set_name(request.name());
    j.set_owner(qEnvironmentVariable("USER").toStdString());
    j.set_attempt(1);
    j.set_result_dir(
        QDir(config_.runRoot).filePath(QString::fromStdString(j.run_id())).toStdString());
    j.set_result_dir_local(true);
    j.set_state(jobs::JOB_STATE_SUBMITTING);
    *j.mutable_labels() = request.labels();
    (*j.mutable_labels())["desktop.solver"] = config_.mock ? "mock_solver" : "chipcae-solve";
    stamp(j.mutable_submitted_at());
    QDir().mkpath(QString::fromStdString(j.result_dir()));
    writeJobFile(path(j, "control.jsonl"), {}); // New UUID only. Reopen never creates/truncates it.
    auto id = j.run_id();
    runs_.emplace(id, std::move(r));
    auto& active = run(id);
    try {
        QJsonObject hashes;
        std::set<QString> visited;
        snapshot(casePath,
                 path(j, "inputs/case.json"),
                 hashes,
                 visited,
                 QString::fromStdString(request.case_file().sha256()));
        snapshot(profilePath, path(j, "run.json"), hashes, visited);
        for (const auto& input : request.inputs()) {
            auto name = QString::fromStdString(input.logical_name());
            if (name.isEmpty() || QDir::isAbsolutePath(name) || name.split('/').contains(".."))
                throw std::runtime_error("Unsafe input logical_name");
            snapshot(QFileInfo(casePath).dir().filePath(name),
                     path(j, "inputs/" + name),
                     hashes,
                     visited,
                     QString::fromStdString(input.sha256()));
        }
        CaseDocument document;
        if (!document.load(path(j, "inputs/case.json")) || !document.validate().empty())
            throw std::runtime_error("CASE_INVALID: snapshot validation failed");
        j.set_case_sha256(document.contentHash().toStdString());
        auto profile = object(readJobFile(path(j, "run.json")));
        auto mapped = ProfileMapper::map(profile, config_.cudaAvailable);
        if (request.has_profile() &&
            request.profile().SerializeAsString() != mapped.SerializeAsString())
            throw std::runtime_error("RunProfile disagrees with JSON profile");
        *j.mutable_profile() = mapped;
        j.mutable_native()->set_scheduler(jobs::SCHEDULER_TYPE_LOCAL);
        writeJobFile(path(j, "snapshot-hashes.json"), QJsonDocument(hashes).toJson());
        writeJobFile(path(j, "submit.pb"), QByteArray::fromStdString(request.SerializeAsString()));
        auto command = ProfileMapper::command(profile,
                                              config_,
                                              path(j, "inputs/case.json"),
                                              path(j, "run.json"),
                                              QString::fromStdString(j.result_dir()));
        active.mpi = command.program == config_.mpi;
        active.worker = std::make_unique<PreviewWorker>();
        connect(active.worker.get(), &PreviewWorker::started, this, [this, id](qint64 pid) {
            auto& r = run(id);
            if (terminal(r.job.state()))
                return;
            r.job.set_state(jobs::JOB_STATE_RUNNING);
            r.job.mutable_native()->set_native_job_id(std::to_string(pid));
            stamp(r.job.mutable_started_at());
            persist(r);
        });
        connect(active.worker.get(),
                &PreviewWorker::errorOccurred,
                this,
                [this, id](QProcess::ProcessError error, const QString& message) {
                    if (error == QProcess::FailedToStart)
                        fail(run(id), jobs::FAILURE_CATEGORY_LAUNCH, message.toStdString());
                });
        connect(
            active.worker.get(),
            &PreviewWorker::finished,
            this,
            [this, id](int code, QProcess::ExitStatus status) {
                auto& r = run(id);
                if (terminal(r.job.state()))
                    return;
                r.job.mutable_native()->set_exit_status_raw(code);
                if (code != 0 || status != QProcess::NormalExit) {
                    fail(r,
                         r.mpi ? jobs::FAILURE_CATEGORY_MPI : jobs::FAILURE_CATEGORY_INTERNAL,
                         "Solver/launcher exited abnormally",
                         code);
                    return;
                }
                bool exitSeen = false;
                QString reason;
                // D02 JSON events are explicitly mock-only; no claim of real kernel
                // integration.
                if (config_.mock && QFileInfo::exists(path(r.job, "mock-events.jsonl"))) {
                    for (auto line : readJobFile(path(r.job, "mock-events.jsonl")).split('\n')) {
                        if (line.isEmpty())
                            continue;
                        auto event = QJsonDocument::fromJson(line).object();
                        if (event.contains("exit")) {
                            auto exit = event.value("exit").toObject();
                            exitSeen = exit.value("exit_code").toInt(-1) == 0;
                            reason = exit.value("reason").toString();
                        }
                    }
                }
                if (!exitSeen) {
                    fail(r,
                         jobs::FAILURE_CATEGORY_JOB_MANAGER,
                         "Missing successful KernelExit",
                         code);
                    return;
                }
                r.job.set_state(jobs::JOB_STATE_COMPLETED);
                r.job.set_completion(reason == "checkpoint_and_stop"
                                         ? jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT
                                     : reason == "user_stop" ? jobs::COMPLETION_KIND_USER_STOP
                                                             : jobs::COMPLETION_KIND_END_REACHED);
                stamp(r.job.mutable_ended_at());
                persist(r);
            });
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("OMP_NUM_THREADS", QString::number(mapped.cpus_per_rank()));
        j.set_state(jobs::JOB_STATE_STARTING);
        stamp(j.mutable_started_at());
        persist(active);
        active.worker->start(command.program, command.arguments, env);
    } catch (const std::exception& e) {
        fail(active, jobs::FAILURE_CATEGORY_CASE_INVALID, e.what());
    }
    return active.job;
}
jobs::Job LocalJobService::Get(const jobs::JobRef& request) const {
    return const_cast<LocalJobService*>(this)->run(request.run_id()).job;
}
jobs::ListJobsResponse LocalJobService::List(const jobs::ListJobsRequest& request) const {
    jobs::ListJobsResponse response;
    for (const auto& [id, r] : runs_) {
        if (!request.page_token().empty() && id <= request.page_token())
            continue;
        if (!request.states().empty() &&
            std::find(request.states().begin(), request.states().end(), r->job.state()) ==
                request.states().end())
            continue;
        if (!request.label_selector().empty()) {
            auto selector = QString::fromStdString(request.label_selector()).split('=');
            if (selector.size() != 2)
                throw std::runtime_error("label_selector must be key=value");
            auto it = r->job.labels().find(selector[0].toStdString());
            if (it == r->job.labels().end() || it->second != selector[1].toStdString())
                continue;
        }
        if (request.page_size() && response.jobs_size() == static_cast<int>(request.page_size())) {
            response.set_next_page_token(response.jobs(response.jobs_size() - 1).run_id());
            break;
        }
        *response.add_jobs() = r->job;
    }
    return response;
}
jobs::ControlReply LocalJobService::Control(const jobs::ControlRequest& request) {
    uuid(request.client_request_id());
    auto& r = run(request.run_id());
    if (auto it = r.controls.find(request.client_request_id()); it != r.controls.end())
        return it->second;
    jobs::ControlReply reply;
    reply.set_delivery(jobs::CONTROL_DELIVERY_REJECTED);
    const bool interval = request.action() == jobs::CONTROL_ACTION_SET_OUTPUT_INTERVAL;
    if (terminal(r.job.state()) || request.action() < jobs::CONTROL_ACTION_PAUSE ||
        request.action() > jobs::CONTROL_ACTION_CHECKPOINT_AND_STOP ||
        (interval && request.arg_case() == jobs::ControlRequest::ARG_NOT_SET) ||
        (!interval && request.arg_case() != jobs::ControlRequest::ARG_NOT_SET) ||
        (request.has_output_interval_time() &&
         (!std::isfinite(request.output_interval_time()) || request.output_interval_time() <= 0)) ||
        (request.has_output_interval_steps() && !request.output_interval_steps())) {
        reply.set_message("State/action/interval does not permit control");
        return reply;
    }
    QJsonObject command;
    command["issuedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    command["cmd_seq"] = QString::number(r.cmdSeq + 1);
    command["client_request_id"] = QString::fromStdString(request.client_request_id());
    auto action = QString::fromStdString(jobs::ControlAction_Name(request.action()));
    command["action"] = action.mid(QString("CONTROL_ACTION_").size());
    if (request.has_output_interval_time())
        command["output_interval_time"] = request.output_interval_time();
    if (request.has_output_interval_steps())
        command["output_interval_steps"] = QString::number(request.output_interval_steps());
    QFile file(path(r.job, "control.jsonl"));
    auto bytes = QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n';
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append) || file.write(bytes) != bytes.size() ||
        !file.flush())
        throw std::runtime_error("Control append failed");
    reply.set_cmd_seq(++r.cmdSeq);
    reply.set_delivery(jobs::CONTROL_DELIVERY_QUEUED);
    r.controls.emplace(request.client_request_id(), reply);
    return reply;
}
jobs::Job LocalJobService::Cancel(const jobs::CancelRequest& request) {
    auto& r = run(request.run_id());
    if (!terminal(r.job.state())) {
        r.job.set_state(jobs::JOB_STATE_CANCELLED);
        stamp(r.job.mutable_ended_at());
        persist(r);
        if (r.worker)
            r.worker->cancel(static_cast<int>(std::clamp<int64_t>(
                request.grace().seconds() * 1000 + request.grace().nanos() / 1000000, 0, 60000)));
    }
    return r.job;
}
jobs::ListOutputsResponse
LocalJobService::ListOutputs(const jobs::ListOutputsRequest& request) const {
    auto job = const_cast<LocalJobService*>(this)->run(request.run_id()).job;
    jobs::ListOutputsResponse result;
    if (!QFileInfo::exists(path(job, "manifest.json")))
        return result;
    auto manifest = object(readJobFile(path(job, "manifest.json")));
    result.set_manifest_path("manifest.json");
    result.set_manifest_generation(manifest.value("generation").toVariant().toULongLong());
    if (request.since_generation() && result.manifest_generation() <= request.since_generation())
        return result;
    for (auto entry : manifest.value("steps").toArray()) {
        auto s = entry.toObject();
        auto window = s.value("n").toVariant().toLongLong();
        if (window <= request.since_window())
            continue;
        auto* step = result.add_steps();
        step->set_window(window);
        step->set_time(s.value("time").toDouble());
        step->set_complete(s.value("complete").toBool(true));
        auto* file = step->add_files();
        file->set_path(s.value("file").toString().toStdString());
        file->set_sha256(s.value("checksum").toString().toStdString());
        for (auto block : s.value("blocks").toArray())
            step->add_blocks(block.toString().toStdString());
    }
    return result;
}
} // namespace chipcae::desktop
