#include "RunMonitor.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <chipcae/run/v1/run.pb.h>
#include <google/protobuf/util/json_util.h>
namespace chipcae::desktop {
RunMonitor::RunMonitor(QString dir, EventFormat mode, QString eventFile)
    : directory(std::move(dir)), format(mode), tailer_(QDir(directory).filePath(eventFile), mode) {}
const std::vector<MonitorLogEntry>& RunMonitor::logEntries() const {
    return logEntries_;
}
void RunMonitor::appendDiagnostic(const QString& text) {
    if (diagnostics.contains(text))
        return;
    diagnostics.append(text);
    logEntries_.push_back({0, 0, text, true});
}
void RunMonitor::poll() {
    auto result = tailer_.poll();
    for (const auto& event : result.events)
        consume(event);
    if (!result.diagnostic.isEmpty())
        appendDiagnostic(result.diagnostic);
    QFile file(QDir(directory).filePath("control.jsonl"));
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        appendDiagnostic("cannot read control.jsonl");
        return;
    }
    if (quint64(file.size()) < commandOffset_)
        commandOffset_ = 0;
    file.seek(commandOffset_);
    while (!file.atEnd()) {
        auto line = file.readLine();
        if (!line.endsWith('\n'))
            break;
        commandOffset_ += line.size();
        QJsonParseError error;
        auto doc = QJsonDocument::fromJson(line, &error);
        auto obj = doc.object();
        chipcae::run::v1::ControlCommand parsed;
        const auto parsedStatus =
            google::protobuf::util::JsonStringToMessage(line.toStdString(), &parsed);
        bool ok = false;
        auto seq = obj.value("cmd_seq").toString().toULongLong(&ok);
        if (!parsedStatus.ok() ||
            parsed.action() == chipcae::run::v1::ControlCommand::ACTION_UNSPECIFIED ||
            error.error != QJsonParseError::NoError || !doc.isObject() || !ok || !seq ||
            obj.value("client_request_id").toString().isEmpty() ||
            obj.value("action").toString().isEmpty()) {
            appendDiagnostic("malformed complete control.jsonl line");
            continue;
        }
        if (seq <= commandSequence_)
            continue;
        commandSequence_ = seq;
        commands.push_back(obj);
    }
}
void RunMonitor::consume(const io::Event& event) {
    std::visit(
        [this](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, io::Progress>)
                progress = x;
            else if constexpr (std::is_same_v<T, io::State>) {
                static const char* names[] = {
                    "Unspecified", "Starting", "Running", "Paused", "Completed", "Failed"};
                const auto value = static_cast<unsigned>(x.value);
                if (value >= sizeof(names) / sizeof(names[0])) {
                    appendDiagnostic("invalid state value");
                    return;
                }
                state = names[value];
                reconcile();
            } else if constexpr (std::is_same_v<T, io::Residual>)
                residuals.push_back(
                    {x.solver, x.field, x.window, {}, x.iteration, x.norm, x.flags});
            else if constexpr (std::is_same_v<T, io::ResidualBatch>) {
                for (const auto& point : x.points)
                    residuals.push_back({point.solver,
                                         point.field,
                                         x.window,
                                         x.time,
                                         point.iteration,
                                         point.norm,
                                         point.flags});
            } else if constexpr (std::is_same_v<T, io::LogLine>) {
                const auto message = QString::fromStdString(x.message);
                logs.append(QString("level=%1 rank=%2 %3").arg(x.level).arg(x.rank).arg(message));
                logEntries_.push_back({x.level, x.rank, message, false});
            } else if constexpr (std::is_same_v<T, io::ControlAck>) {
                if (pendingSequence && x.cmd_seq == pendingSequence &&
                    x.client_request_id == pendingRequest.toStdString() && x.status >= 1 &&
                    x.status <= 3) {
                    ack = x;
                    stopStatus = QStringList{"", "APPLIED", "NOOP", "REJECTED"}[x.status];
                }
            } else if constexpr (std::is_same_v<T, io::KernelExit>) {
                exit_ = x;
                reconcile();
            } else if constexpr (std::is_same_v<T, io::OutputAvailable>) {
                ++manifestLookups;
                QFile file(QDir(directory).filePath("manifest.json"));
                if (file.open(QIODevice::ReadOnly)) {
                    QJsonParseError error;
                    auto doc = QJsonDocument::fromJson(file.readAll(), &error);
                    if (error.error == QJsonParseError::NoError && doc.isObject())
                        manifest = doc.object();
                    else
                        appendDiagnostic("invalid output manifest");
                }
            }
        },
        event.payload);
}
void RunMonitor::reconcile() {
    if (!processCode_ || !exit_) {
        if (processCode_ || exit_)
            state = "Awaiting exit reconciliation";
        return;
    }
    state = (*processCode_ == 0 && exit_->exit_code == 0) ? "Completed" : "Failed";
}
void RunMonitor::processExited(int code) {
    processCode_ = code;
    reconcile();
}
void RunMonitor::trackStop(quint64 seq, QString id) {
    pendingSequence = seq;
    pendingRequest = std::move(id);
    ack.reset();
    timedOut_ = false;
    stopStatus = "Pending";
}
void RunMonitor::stop(LocalJobService& broker, const QString& runId) {
    jobs::ControlRequest request;
    request.set_run_id(runId.toStdString());
    auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.set_client_request_id(id.toStdString());
    request.set_action(jobs::CONTROL_ACTION_STOP);
    try {
        auto reply = broker.Control(request);
        if (reply.delivery() == jobs::CONTROL_DELIVERY_QUEUED)
            trackStop(reply.cmd_seq(), id);
        else {
            stopStatus = "Delivery rejected";
            appendDiagnostic(QString::fromStdString(reply.message()));
        }
    } catch (const std::exception& e) {
        appendDiagnostic(QString::fromUtf8(e.what()));
    }
}
void RunMonitor::checkTimeout(qint64 elapsedMs) {
    if (pendingSequence && !ack && elapsedMs >= 30000 && !timedOut_) {
        timedOut_ = true;
        appendDiagnostic("STOP ACK timeout; request remains pending");
    }
}
} // namespace chipcae::desktop
