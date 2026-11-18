#pragma once
#include "EventTailer.hpp"
#include "LocalJobService.hpp"

#include <QJsonObject>
#include <QStringList>
#include <optional>
namespace chipcae::desktop {
struct ResidualSample {
    std::string solver, field;
    quint64 window;
    std::optional<double> time;
    quint32 iteration;
    double norm;
    quint32 flags;
};
struct MonitorLogEntry {
    qint32 level = 0;
    qint32 rank = 0;
    QString message;
    bool diagnostic = false;
};
class RunMonitor {
public:
    RunMonitor(QString directory, EventFormat format, QString eventFile = "events.caelog");
    void poll();
    void consume(const io::Event& event);
    void processExited(int code);
    void stop(LocalJobService& broker, const QString& runId);
    void trackStop(quint64 sequence, QString requestId);
    void checkTimeout(qint64 elapsedMs);
    // Test-first stub. Structured entries are populated by the D18 implementation.
    const std::vector<MonitorLogEntry>& logEntries() const {
        static const std::vector<MonitorLogEntry> empty;
        return empty;
    }
    QString directory;
    EventFormat format;
    QString state = "Waiting", stopStatus = "Idle";
    io::Progress progress;
    QStringList logs, diagnostics;
    std::vector<ResidualSample> residuals;
    std::vector<QJsonObject> commands;
    QJsonObject manifest;
    std::optional<io::ControlAck> ack;
    quint64 manifestLookups = 0;
    quint64 pendingSequence = 0;
    QString pendingRequest;

private:
    void reconcile();
    EventTailer tailer_;
    quint64 commandOffset_ = 0, commandSequence_ = 0;
    std::optional<int> processCode_;
    std::optional<io::KernelExit> exit_;
    bool timedOut_ = false;
};
} // namespace chipcae::desktop
