#pragma once
#include "JobTypes.hpp"

#include <QProcessEnvironment>

namespace chipcae::desktop {
struct DesktopEntrypoint {
    QString source = "real";
    QString program;
    QStringList prefix;
    QString materialMap;
};
struct DesktopPrecomputed {
    QString runDir;
    QString manifestSha256;
};
class DesktopConfig {
public:
    // Throws std::runtime_error on invalid arguments, files, or environment.
    static DesktopConfig
    load(const QStringList& args,
         const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment());
    // Independent of load() and current(). Returns an absolute directory containing the
    // schema marker, or an empty string on an implicit miss. Invalid overrides throw.
    static QString
    locateSchemaDir(const QStringList& args,
                    const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment(),
                    const QString& executableDir = QString(),
                    const QString& compileTimeFallback = QString());
    // Joins a basename to the located directory. Only the schema marker must exist.
    static QString
    locateSchemaFile(const QString& fileName,
                     const QStringList& args,
                     const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment(),
                     const QString& executableDir = QString(),
                     const QString& compileTimeFallback = QString());
    static void setCurrent(const DesktopConfig& config);
    static const DesktopConfig& current();
    const DesktopEntrypoint& importEntry() const { return import_; }
    const DesktopEntrypoint& previewEntry() const { return preview_; }
    const DesktopEntrypoint& solveEntry() const { return solve_; }
    const DesktopPrecomputed& precomputed() const { return precomputed_; }
    const QString& runRoot() const { return runRoot_; }
    const QString& eventsFile() const { return eventsFile_; }
    const QString& mpi() const { return mpi_; }
    LauncherConfig launcherConfig() const;

private:
    DesktopConfig();
    DesktopEntrypoint import_, preview_, solve_;
    DesktopPrecomputed precomputed_;
    QString runRoot_, eventsFile_ = "events.bin", mpi_ = "mpirun";
};
} // namespace chipcae::desktop
