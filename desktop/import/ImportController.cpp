#include "ImportController.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>

namespace chipcae::desktop {
namespace {
QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
} // namespace
ImportController::ImportController(CaseDocument& document, QObject* parent)
    : QObject(parent), document_(document) {
    connect(&worker_, &PreviewWorker::stderrLine, this, [this](const QByteArray& line) {
        stderr_ += line + '\n';
    });
    connect(&worker_, &PreviewWorker::finished, this, &ImportController::complete);
    connect(&worker_,
            &PreviewWorker::errorOccurred,
            this,
            [this](QProcess::ProcessError error, const QString& message) {
                error_ = message;
                if (error == QProcess::FailedToStart)
                    complete(-1, QProcess::CrashExit);
            });
}
ImportController::~ImportController() {
    // PreviewWorker stops its process during destruction. Its final signals must
    // not access controller members that have already been destroyed.
    disconnect(&worker_, nullptr, this, nullptr);
}
bool ImportController::start(const QString& input,
                             const QString& output,
                             const QString& polyset,
                             const QString& report,
                             const QString& program,
                             const QStringList& prefix) {
    if (busy_ || output.isEmpty())
        return false;
    QSet<QString> paths;
    for (const auto& path : {output, polyset, report}) {
        if (path.isEmpty())
            continue;
        auto absolute = QFileInfo(path).absoluteFilePath();
        if (QFileInfo::exists(path) || paths.contains(absolute) ||
            absolute == QFileInfo(input).absoluteFilePath())
            return false;
        paths.insert(absolute);
    }
    staging_ = std::make_unique<QTemporaryDir>(
        QFileInfo(output).absoluteDir().filePath(".chipcae-import-XXXXXX"));
    if (!staging_->isValid())
        return false;
    destinations_ = {output, polyset, report};
    staged_ = {staging_->filePath("out.stack.json"),
               staging_->filePath("out.polyset"),
               staging_->filePath("defects.json")};
    QStringList arguments = prefix;
    arguments << input << "-o" << staged_[0];
    if (!polyset.isEmpty())
        arguments << "--polyset" << staged_[1];
    // Always request a report for UI grading, even if no exported report was requested.
    arguments << "--report" << staged_[2];
    stderr_.clear();
    report_.clear();
    grade_.clear();
    error_.clear();
    exitCode_ = -1;
    cancelled_ = awaitingConfirmation_ = false;
    busy_ = true;
    return worker_.start(program, arguments);
}
void ImportController::cancel() {
    if (busy_) {
        cancelled_ = true;
        worker_.cancel(100);
    }
}
void ImportController::confirmDefects() {
    if (awaitingConfirmation_) {
        awaitingConfirmation_ = false;
        emit userConfirmed();
    }
}
void ImportController::complete(int code, QProcess::ExitStatus status) {
    if (!busy_)
        return;
    exitCode_ = code;
    exitStatus_ = status;
    report_ = readFile(staged_[2]);
    try {
        auto parsed = Json::parse(report_.toStdString());
        grade_ = QString::fromStdString(parsed.at("grade").as<std::string>());
        if ((grade_ != "A" && grade_ != "B" && grade_ != "C") || !parsed.at("defects").is_array())
            throw std::runtime_error("Invalid report structure");
    } catch (const std::exception&) {
        if (error_.isEmpty())
            error_ = "Missing or corrupt defects report";
        grade_.clear();
    }
    bool replaced = false;
    if (!cancelled_ && code == 0 && status == QProcess::NormalExit && grade_ == "A") {
        try {
            const auto stack = Json::parse(readFile(staged_[0]).toStdString());
            // Validate before exporting any files or modifying the active document.
            if (!document_.checkImportedStack(stack))
                throw std::runtime_error("Imported stack failed CaseDocument validation");
            QStringList published;
            for (int i = 0; i < destinations_.size(); ++i) {
                if (destinations_[i].isEmpty())
                    continue;
                if (!QFile::copy(staged_[i], destinations_[i])) {
                    for (const auto& path : published)
                        QFile::remove(path);
                    throw std::runtime_error("Cannot publish import outputs");
                }
                published << destinations_[i];
            }
            replaced = document_.replaceImportedStack(stack);
        } catch (const std::exception& e) {
            error_ = QString::fromUtf8(e.what());
        }
    }
    awaitingConfirmation_ = !cancelled_ && status == QProcess::NormalExit && grade_ == "B";
    staging_.reset();
    busy_ = false;
    if (!cancelled_)
        emit defectsReady();
    emit finished(replaced);
}
} // namespace chipcae::desktop
