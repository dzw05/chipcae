#pragma once
#include <QJsonObject>
#include <QStringList>
#include <chipcae/jobs/v1/jobs.pb.h>
namespace chipcae::desktop {
namespace jobs = chipcae::jobs::v1;
struct LauncherConfig {
    QString runRoot;
    QString solver = "chipcae-solve";
    QString mpi = "mpirun";
    bool cudaAvailable = false;
    bool mock = false;
};
struct LaunchCommand {
    QString program;
    QStringList arguments;
};
bool terminal(jobs::JobState state);
QByteArray readJobFile(const QString& path);
void writeJobFile(const QString& path, const QByteArray& bytes);
QString sha256(const QByteArray& bytes);
} // namespace chipcae::desktop
