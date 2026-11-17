#include "JobTypes.hpp"

#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>
#include <stdexcept>
namespace chipcae::desktop {
bool terminal(jobs::JobState state) {
    return state == jobs::JOB_STATE_COMPLETED || state == jobs::JOB_STATE_FAILED ||
           state == jobs::JOB_STATE_CANCELLED;
}
QByteArray readJobFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw std::runtime_error(f.errorString().toStdString());
    return f.readAll();
}
void writeJobFile(const QString& path, const QByteArray& bytes) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() || !f.commit())
        throw std::runtime_error("Cannot persist job file");
}
QString sha256(const QByteArray& bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
} // namespace chipcae::desktop
