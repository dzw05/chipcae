#include "EventTailer.hpp"
#include "LocalJobService.hpp"
#include "ProfileMapper.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <iostream>
using namespace chipcae::desktop;
namespace {
void check(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
std::string uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
}
jobs::Job wait(LocalJobService& service, const jobs::Job& job) {
    jobs::JobRef ref;
    ref.set_run_id(job.run_id());
    QElapsedTimer timer;
    timer.start();
    while (!terminal(service.Get(ref).state()) && timer.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    check(terminal(service.Get(ref).state()), "job timeout");
    return service.Get(ref);
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    // The test executable also acts as a solver, using the production event writer.
    if (app.arguments().contains("--out")) {
        auto args = app.arguments();
        auto out = args.at(args.indexOf("--out") + 1);
        auto profile =
            QJsonDocument::fromJson(readJobFile(args.at(args.indexOf("--profile") + 1))).object();
        auto mode = profile["x-fixture"].toString();
        auto file = out + "/" + profile["x-events-file"].toString("events.bin");
        if (mode != "missing") {
            {
                chipcae::io::EventLogWriter writer(file.toStdString());
                writer.append({1, 0, chipcae::io::KernelHello{}});
                if (mode != "no-exit")
                    writer.append({2,
                                   0,
                                   chipcae::io::KernelExit{
                                       mode == "failed-exit" ? 7 : 0,
                                       2,
                                       profile["x-reason"].toString("end_reached").toStdString()}});
            }
            if (mode == "torn" || mode == "invalid") {
                auto bytes = readJobFile(file);
                bytes += mode == "torn" ? QByteArray("CAE") : bytes;
                if (mode == "invalid")
                    bytes[bytes.size() - 1] ^= 1;
                writeJobFile(file, bytes);
            }
        }
        return mode == "abnormal" ? 9 : 0;
    }
    try {
        QTemporaryDir temp;
        check(temp.isValid(), "temp directory");
        auto root = temp.filePath("runs with spaces");
        auto adapter = temp.filePath("mock solver");
        writeJobFile(adapter,
                     QByteArray("#!/bin/sh\nexec python3 ") +
                         "'" CHIPCAE_REPOSITORY "/desktop/tools/mock_solver.py' \"$@\"\n");
        QFile::setPermissions(adapter, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        LauncherConfig config{root, adapter, "mpirun", false, true};
        auto profilePath = temp.filePath("run profile.json");
        QJsonObject profile{{"schema", "chipcae.run_profile/1.0"},
                            {"ranks", 1},
                            {"backend", "cpu"},
                            {"gpus", 0},
                            {"threads_per_rank", 3},
                            {"launcher", "direct"},
                            {"x-mock-delay", 0.05},
                            {"x-mock-windows", 10}};
        auto saveProfile = [&] { writeJobFile(profilePath, QJsonDocument(profile).toJson()); };
        saveProfile();
        auto mapped = ProfileMapper::map(profile, false);
        check(mapped.cpus_per_rank() == 3 && mapped.env().at("OMP_NUM_THREADS") == "3", "mapping");
        auto mpiProfile = profile;
        mpiProfile["launcher"] = "mpirun";
        mpiProfile["ranks"] = 2;
        mpiProfile["mpi_args"] = QJsonArray{"--host", "host with spaces", "$(false)"};
        auto cmd = ProfileMapper::command(mpiProfile, config, "a case", "a profile", "an out");
        check(cmd.program == "mpirun" && cmd.arguments == QStringList{"-n",
                                                                      "2",
                                                                      "--host",
                                                                      "host with spaces",
                                                                      "$(false)",
                                                                      adapter,
                                                                      "--case",
                                                                      "a case",
                                                                      "--profile",
                                                                      "a profile",
                                                                      "--out",
                                                                      "an out"},
              "MPI argv tokens");
        for (auto bad : {QJsonObject{{"backend", "cuda"}, {"gpus", 1}},
                         QJsonObject{{"gpus", 1}},
                         QJsonObject{{"ranks", 2}}}) {
            auto p = profile;
            for (auto it = bad.begin(); it != bad.end(); ++it)
                p[it.key()] = it.value();
            bool rejected = false;
            try {
                ProfileMapper::map(p, false);
            } catch (...) {
                rejected = true;
            }
            check(rejected, "profile capability/direct rejection");
        }
        jobs::SubmitRequest request;
        request.set_client_request_id(uuid());
        request.set_name("explicit mock");
        auto casePath = QStringLiteral(CHIPCAE_REPOSITORY "/schema/examples/fcbga_demo.case.json");
        request.mutable_case_file()->set_sha256(sha256(readJobFile(casePath)).toStdString());
        jobs::ControlRequest stop;
        jobs::Job original;
        QByteArray journal;
        {
            LocalJobService service(config);
            bool locked = false;
            try {
                LocalJobService second(config);
            } catch (...) {
                locked = true;
            }
            check(locked, "single writer lock");
            original = service.Submit(request, casePath, profilePath);
            check(original.state() == jobs::JOB_STATE_STARTING,
                  original.failure().message().c_str());
            check(service.Submit(request, "missing", "missing").run_id() == original.run_id(),
                  "submit UUID idempotency");
            stop.set_run_id(original.run_id());
            stop.set_client_request_id(uuid());
            stop.set_action(jobs::CONTROL_ACTION_STOP);
            auto reply = service.Control(stop);
            check(reply.cmd_seq() == 1 && service.Control(stop).cmd_seq() == 1,
                  "control idempotency");
            auto second = stop;
            second.set_client_request_id(uuid());
            check(service.Control(second).cmd_seq() == 2, "monotonic cmd seq");
            auto done = wait(service, original);
            check(done.state() == jobs::JOB_STATE_COMPLETED &&
                      done.completion() == jobs::COMPLETION_KIND_USER_STOP,
                  "STOP completion");
            auto out = QString::fromStdString(done.result_dir());
            journal = readJobFile(out + "/control.jsonl");
            auto capture = QJsonDocument::fromJson(readJobFile(out + "/mock-argv.json")).object();
            auto argv = capture["argv"].toArray();
            check(argv.size() == 6 && argv[0] == "--case" && argv[2] == "--profile" &&
                      argv[4] == "--out" && capture["omp"] == "3",
                  "mock official argv/env capture");
            check(QFileInfo::exists(out + "/snapshot-hashes.json"), "snapshot hashes");
            check(readJobFile(out + "/inputs/case.json") == readJobFile(casePath),
                  "immutable exact case bytes");
            writeJobFile(
                out + "/manifest.json",
                R"({"generation":7,"steps":[{"n":2,"time":1,"file":"result.vtkhdf","checksum":"abc","complete":true}]})");
            jobs::ListOutputsRequest outputs;
            outputs.set_run_id(done.run_id());
            outputs.set_since_window(-1);
            check(service.ListOutputs(outputs).steps_size() == 1 &&
                      service.ListOutputs(outputs).manifest_generation() == 7,
                  "manifest authoritative outputs");
            auto launch = request;
            launch.set_client_request_id(uuid());
            profile["x-mock-exit"] = 9;
            saveProfile();
            auto failed = wait(service, service.Submit(launch, casePath, profilePath));
            check(failed.failure().category() == jobs::FAILURE_CATEGORY_INTERNAL, "abnormal exit");
            profile.remove("x-mock-exit");
            profile["x-mock-missing-exit"] = true;
            saveProfile();
            launch.set_client_request_id(uuid());
            failed = wait(service, service.Submit(launch, casePath, profilePath));
            check(failed.failure().category() == jobs::FAILURE_CATEGORY_JOB_MANAGER,
                  "missing KernelExit");
            profile.remove("x-mock-missing-exit");
            saveProfile();
            launch.set_client_request_id(uuid());
            auto cancelled = service.Submit(launch, casePath, profilePath);
            jobs::CancelRequest cancel;
            cancel.set_run_id(cancelled.run_id());
            check(service.Cancel(cancel).state() == jobs::JOB_STATE_CANCELLED &&
                      service.Cancel(cancel).completion() == jobs::COMPLETION_KIND_UNSPECIFIED,
                  "hard cancel differs from STOP");
            launch.set_client_request_id(uuid());
            launch.mutable_case_file()->set_sha256(std::string(64, '0'));
            check(service.Submit(launch, casePath, profilePath).failure().message() ==
                      "INPUT_HASH_MISMATCH",
                  "hash mismatch blocked");
        }
        {
            LocalJobService reopened(config);
            check(reopened.Control(stop).cmd_seq() == 1, "reopened control idempotency");
            check(reopened.Submit(request, "missing", "missing").run_id() == original.run_id(),
                  "reopened submit idempotency");
            check(readJobFile(QString::fromStdString(original.result_dir()) + "/control.jsonl") ==
                      journal,
                  "reopen does not truncate");
        }
        config.runRoot = temp.filePath("launch failure");
        config.solver = "/missing/solver";
        {
            LocalJobService service(config);
            request.set_client_request_id(uuid());
            check(wait(service, service.Submit(request, casePath, profilePath))
                          .failure()
                          .category() == jobs::FAILURE_CATEGORY_LAUNCH,
                  "launch failure category");
        }
        config.runRoot = temp.filePath("MPI failure");
        config.mpi = "/bin/false";
        profile["launcher"] = "mpirun";
        saveProfile();
        {
            LocalJobService service(config);
            request.set_client_request_id(uuid());
            check(wait(service, service.Submit(request, casePath, profilePath))
                          .failure()
                          .category() == jobs::FAILURE_CATEGORY_MPI,
                  "MPI failure category");
        }
        profile["launcher"] = "direct";
        config.mock = false;
        config.solver = QCoreApplication::applicationFilePath();
        for (const auto& mode :
             {"success", "missing", "torn", "invalid", "failed-exit", "no-exit", "abnormal"}) {
            config.runRoot = temp.filePath(QString("real-") + mode);
            profile["x-fixture"] = mode;
            saveProfile();
            LocalJobService service(config);
            request.set_client_request_id(uuid());
            auto done = wait(service, service.Submit(request, casePath, profilePath));
            if (QString(mode) == "success") {
                check(done.state() == jobs::JOB_STATE_COMPLETED &&
                          done.completion() == jobs::COMPLETION_KIND_END_REACHED,
                      "real KernelExit must complete job");
                EventTailer tailer(QString::fromStdString(done.result_dir()) + "/events.bin",
                                   EventFormat::LegacyCaelog1);
                auto decoded = tailer.poll();
                check(decoded.diagnostic.isEmpty() && decoded.events.size() == 2 &&
                          std::get<chipcae::io::KernelExit>(decoded.events.back().payload)
                                  .exit_code == 0,
                      "launcher and monitor share successful KernelExit bytes");
            } else {
                check(done.state() == jobs::JOB_STATE_FAILED &&
                          done.completion() == jobs::COMPLETION_KIND_UNSPECIFIED,
                      "invalid/missing KernelExit must fail");
                check(done.failure().category() == (QString(mode) == "abnormal"
                                                        ? jobs::FAILURE_CATEGORY_INTERNAL
                                                        : jobs::FAILURE_CATEGORY_JOB_MANAGER),
                      "event failure differs from abnormal process exit");
            }
        }
        for (bool mock : {false, true}) {
            for (const auto& name : {"", "custom events.bin"}) {
                for (const auto& reason : {"end_reached", "user_stop", "checkpoint_and_stop"}) {
                    config.mock = mock;
                    config.eventsFile = name;
                    config.runRoot =
                        temp.filePath(QString("events-%1-%2-%3").arg(mock).arg(name).arg(reason));
                    profile["x-fixture"] = "success";
                    profile["x-events-file"] =
                        QString(name).isEmpty() ? (mock ? "events.caelog" : "events.bin") : name;
                    profile["x-reason"] = reason;
                    saveProfile();
                    LocalJobService service(config);
                    request.set_client_request_id(uuid());
                    auto done = wait(service, service.Submit(request, casePath, profilePath));
                    auto expected = QString(reason) == "user_stop" ? jobs::COMPLETION_KIND_USER_STOP
                                    : QString(reason) == "checkpoint_and_stop"
                                        ? jobs::COMPLETION_KIND_STOP_AFTER_CHECKPOINT
                                        : jobs::COMPLETION_KIND_END_REACHED;
                    check(done.state() == jobs::JOB_STATE_COMPLETED &&
                              done.completion() == expected && !done.has_failure(),
                          "event filename defaults/override and completion reason");
                    EventTailer tailer(QString::fromStdString(done.result_dir()) + "/" +
                                           profile["x-events-file"].toString(),
                                       EventFormat::LegacyCaelog1);
                    auto decoded = tailer.poll();
                    check(decoded.events.size() == 2 &&
                              std::get<chipcae::io::KernelExit>(decoded.events.back().payload)
                                      .reason == reason,
                          "monitor and launcher agree on completion reason");
                }
            }
        }
        std::cout << "local launcher mock and real checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
