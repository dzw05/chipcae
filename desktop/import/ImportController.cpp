#include "ImportController.hpp"

#include "ImportSourceDialog.hpp"

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
    : QObject(parent), copyOutput_([](const QString& source, const QString& destination) {
          return QFile::copy(source, destination);
      }),
      document_(document) {
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
    ImportRequest request;
    request.input = input;
    request.output = output;
    request.polyset = polyset;
    request.report = report;
    request.program = program;
    request.prefix = prefix;
    return start(request);
}
bool ImportController::start(const ImportRequest& request) {
    if (busy_)
        return false;
    report_.clear();
    reportCode_.clear();
    grade_.clear();
    stderr_.clear();
    error_.clear();
    exitCode_ = -1;
    exitStatus_ = QProcess::NormalExit;
    outcome_ = ImportOutcome::Failed;
    const auto& input = request.input;
    const auto& output = request.output;
    const auto& polyset = request.polyset;
    const auto& report = request.report;
    if (request.kind == ImportKind::Ecad && request.materialMap.isEmpty()) {
        error_ = "material map required";
        return false;
    }
    if (input.isEmpty() ||
        (request.kind == ImportKind::Ecad ? !ImportSourceDialog::isSupportedImportPath(input)
                                          : !QFileInfo(input).isFile())) {
        error_ = "unsupported import input";
        return false;
    }
    if (output.isEmpty()) {
        error_ = "output required";
        return false;
    }
    QSet<QString> paths;
    for (const auto& path : {output, polyset, report}) {
        if (path.isEmpty())
            continue;
        auto absolute = QFileInfo(path).absoluteFilePath();
        if (QFileInfo::exists(path) || paths.contains(absolute) ||
            absolute == QFileInfo(input).absoluteFilePath() ||
            (request.kind == ImportKind::Ecad && !request.materialMap.isEmpty() &&
             absolute == QFileInfo(request.materialMap).absoluteFilePath())) {
            error_ = "import destination already exists or collides with another path";
            return false;
        }
        paths.insert(absolute);
    }
    staging_ = std::make_unique<QTemporaryDir>(
        QFileInfo(output).absoluteDir().filePath(".chipcae-import-XXXXXX"));
    if (!staging_->isValid()) {
        error_ = "import staging directory creation failed";
        return false;
    }
    destinations_ = {output, polyset, report};
    staged_ = {staging_->filePath("out.stack.json"),
               staging_->filePath("out.polyset"),
               staging_->filePath("defects.json")};
    // Keep caller input and mapping paths unchanged; only generated artifacts
    // belong in the private staging directory until validation succeeds.
    auto stagedRequest = request;
    stagedRequest.output = staged_[0];
    stagedRequest.polyset = polyset.isEmpty() ? QString{} : staged_[1];
    // Every import requests a private report explicitly; destinations_ alone
    // controls whether it is exported to the caller.
    stagedRequest.report = staged_[2];
    const auto arguments = request.prefix + argumentsFor(stagedRequest);
    cancelled_ = false;
    busy_ = true;
    outcome_ = ImportOutcome::Running;
    if (!worker_.start(request.program, arguments)) {
        busy_ = false;
        outcome_ = ImportOutcome::Failed;
        error_ = "Cannot start import process";
        staging_.reset();
        return false;
    }
    request_ = request;
    return true;
}

QStringList ImportController::argumentsFor(const ImportRequest& request) {
    QStringList arguments;
    if (request.kind == ImportKind::Assembly) {
        arguments << "--assembly" << request.input << "-o" << request.output;
        if (!request.report.isEmpty())
            arguments << "--report" << request.report;
        if (!request.polyset.isEmpty())
            arguments << "--polyset" << request.polyset;
    } else {
        arguments << request.input << "-o" << request.output << "--material-map"
                  << request.materialMap;
        if (!request.polyset.isEmpty())
            arguments << "--polyset" << request.polyset;
        if (!request.report.isEmpty())
            arguments << "--report" << request.report;
    }
    if (request.acceptBTier)
        arguments << "--accept-b-tier";
    return arguments;
}

void ImportController::cancel() {
    if (busy_) {
        cancelled_ = true;
        worker_.cancel(100);
    }
}
bool ImportController::restartAccepted() {
    if (busy_ || outcome_ != ImportOutcome::NeedsConfirmation)
        return false;
    auto request = request_;
    request.acceptBTier = true;
    emit userConfirmed();
    const bool started = start(request);
    // Refresh the dialog on local rejection; finished belongs to process completion.
    if (!started)
        emit defectsReady();
    return started;
}
void ImportController::confirmDefects() {
    restartAccepted();
}
bool ImportController::needsMaterialMapReselect() const {
    return !busy_ && exitStatus_ == QProcess::NormalExit && exitCode_ == 9 &&
           reportCode_ == "MATERIAL_MAPPING_MISSING";
}
void ImportController::complete(int code, QProcess::ExitStatus status) {
    if (!busy_)
        return;
    exitCode_ = code;
    exitStatus_ = status;
    report_ = readFile(staged_[2]);
    reportCode_.clear();
    grade_.clear();
    bool validReport = false;
    try {
        auto parsed = Json::parse(report_.toStdString());
        if (!parsed.is_object() || !parsed.at("code").is_string() ||
            !parsed.at("message").is_string() || !parsed.at("defects").is_array())
            throw std::runtime_error("Invalid import report structure");
        const auto reportCode = QString::fromStdString(parsed.at("code").as<std::string>());
        QString grade;
        if (parsed.contains("grade") && parsed.at("grade").is_string())
            grade = QString::fromStdString(parsed.at("grade").as<std::string>());
        reportCode_ = reportCode;
        grade_ = grade;
        validReport = true;
    } catch (const std::exception&) {
    }
    if (cancelled_ || status != QProcess::NormalExit) {
        reportCode_.clear();
        grade_.clear();
    }
    outcome_ = ImportOutcome::Failed;
    if (!cancelled_ && status == QProcess::NormalExit) {
        if (code == 0 && validReport && reportCode_ == "OK")
            outcome_ = ImportOutcome::Accepted;
        else if (validReport && code == 6 && reportCode_ == "IMPORT_CONFIRMATION_REQUIRED")
            outcome_ = ImportOutcome::NeedsConfirmation;
        else if (validReport && ((code == 7 && reportCode_ == "IMPORT_REJECTED") ||
                                 (code == 9 && reportCode_ == "MATERIAL_MAPPING_MISSING")))
            outcome_ = ImportOutcome::Rejected;
    }
    if (outcome_ == ImportOutcome::Failed && error_.isEmpty())
        error_ = cancelled_ ? "Import cancelled"
                            : "exit/report mismatch: missing, corrupt or incompatible report";
    bool replaced = false;
    if (outcome_ == ImportOutcome::Accepted) {
        QStringList published;
        try {
            const auto stack = Json::parse(readFile(staged_[0]).toStdString());
            // Validate before exporting any files or modifying the active document.
            if (!document_.checkImportedStack(stack))
                throw std::runtime_error("Imported stack failed CaseDocument validation");
            for (int i = 0; i < destinations_.size(); ++i) {
                if (destinations_[i].isEmpty())
                    continue;
                const QFileInfo destination(destinations_[i]);
                const bool existed = destination.exists() || destination.isSymLink();
                // Preflight cannot reserve paths while the importer runs. Never
                // publish over a destination that has since appeared externally.
                if (existed)
                    throw std::runtime_error("Cannot publish import outputs");
                // Copy into an owned directory on the destination filesystem. A
                // failed copy can only leave private partial data; the final rename
                // refuses an external destination even if it appears during copying.
                QTemporaryDir publication(
                    destination.absoluteDir().filePath(".chipcae-import-publish-XXXXXX"));
                if (!publication.isValid())
                    throw std::runtime_error("Cannot publish import outputs");
                const auto pending = publication.filePath("output");
                if (!copyOutput_(staged_[i], pending) || !QFile::rename(pending, destinations_[i]))
                    throw std::runtime_error("Cannot publish import outputs");
                // Only a successful no-overwrite rename establishes ownership.
                published << destinations_[i];
            }
            replaced = document_.replaceImportedStack(stack);
            if (!replaced)
                throw std::runtime_error("Cannot replace imported stack");
        } catch (const std::exception& e) {
            for (const auto& path : published)
                QFile::remove(path);
            error_ = QString::fromUtf8(e.what());
            outcome_ = ImportOutcome::Failed;
        }
    }
    staging_.reset();
    busy_ = false;
    if (!cancelled_)
        emit defectsReady();
    emit finished(replaced);
}
} // namespace chipcae::desktop
