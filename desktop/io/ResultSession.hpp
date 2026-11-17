#pragma once

#include "chipcae/io/event_log.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <vtkSmartPointer.h>

class vtkDataObject;
namespace chipcae::desktop {
struct EventLogResult {
    std::vector<chipcae::io::Event> events;
    quint64 validatedBytes = 0;
    QString error;
};
// Synchronous, UI-independent reader. Call on a worker thread for large results;
// each session belongs to one thread. Returned VTK objects own their data.
class ResultSession {
public:
    bool open(const QString& input, const QString& manifest = {}, const QString& eventLog = {});
    vtkSmartPointer<vtkDataObject> loadStep(qint64 n, const QString& field = {});
    static EventLogResult readEventLog(const QString& path, int format = 1);
    const QJsonObject& manifest() const { return manifest_; }
    QJsonArray steps() const;
    QJsonArray fields() const;
    QJsonArray blocks() const;
    QJsonArray summary() const;
    QJsonObject meshDigest() const;
    const QStringList& warnings() const { return warnings_; }
    const QString& error() const { return error_; }
    const QMap<QString, QStringList>& nameTables() const { return names_; }
    const EventLogResult& eventLog() const { return log_; }
    // cell_kind=2 stays in raw data; views should hide these cells by default.
    static constexpr int hiddenCellKind = 2;

private:
    QString checkedPath(const QString& relative) const;
    bool preflight(const QString& file);
    QJsonObject manifest_;
    QString root_, direct_, error_;
    QStringList warnings_;
    QMap<QString, QStringList> names_;
    EventLogResult log_;
};
} // namespace chipcae::desktop
