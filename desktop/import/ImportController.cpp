#include "ImportController.hpp"

#include "ImportSourceDialog.hpp"

#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <cmath>

namespace chipcae::desktop {
namespace {
QByteArray readFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}
// Field checks follow the frozen I3 report contract; additive keys are retained.
void field(bool valid, const std::string& name) {
    if (!valid)
        throw std::runtime_error("invalid field: " + name);
}
bool unsignedInteger(const Json& value) {
    return value.is_uint64() || (value.is_int64() && value.as<int64_t>() >= 0);
}
bool number(const Json& value) {
    return value.is_number() && !value.is_bool() && std::isfinite(value.as<double>());
}
void stringArray(const Json& value, const std::string& name) {
    field(value.is_array(), name);
    for (const auto& item : value.array_range())
        field(item.is_string(), name);
}
void validateDefects(const Json& defects) {
    field(defects.is_array(), "defects");
    for (const auto& defect : defects.array_range()) {
        field(defect.is_object(), "defects[]");
        for (const auto* key : {"defect_id",
                                "tier",
                                "kind",
                                "dimension",
                                "size_unit",
                                "suggestion",
                                "action",
                                "rejection_reason"})
            field(defect.at(key).is_string(), std::string("defects[].") + key);
        field(!defect.at("defect_id").as<std::string>().empty(), "defects[].defect_id");
        const auto& tier = defect.at("tier");
        field(tier == "Weld" || tier == "A" || tier == "B" || tier == "C", "defects[].tier");
        const auto& unit = defect.at("size_unit");
        field(unit == "1" || unit == "m" || unit == "m2" || unit == "m3", "defects[].size_unit");
        field(unsignedInteger(defect.at("reason")), "defects[].reason");
        const auto& size = defect.at("size");
        field(size.is_null() || number(size), "defects[].size");
        field(defect.at("material_ambiguous").is_bool(), "defects[].material_ambiguous");
    }
}
void validateSidecar(const Json& report, bool assembly) {
    for (const auto* key : {"L", "t_min", "eps_weld", "tau_A", "tau_B", "delta_f"})
        field(number(report.at(key)), key);
    const auto& counts = report.at("tier_counts");
    field(counts.is_object(), "tier_counts");
    for (const auto* key : {"Weld", "A", "B", "C"})
        field(unsignedInteger(counts.at(key)), std::string("tier_counts.") + key);
    const auto& accepted = report.at("b_tier_accepted");
    field(accepted.is_null() || (accepted.is_string() && accepted == "all"), "b_tier_accepted");
    validateDefects(report.at("defects"));
    if (assembly || report.contains("parts")) {
        const auto& parts = report.at("parts");
        field(parts.is_object(), "parts");
        const auto& splits = parts.at("split_layers");
        field(splits.is_object(), "parts.split_layers");
        for (const auto& entry : splits.object_range())
            stringArray(entry.value(), "parts.split_layers[]");
        const auto& rewrites = parts.at("rewrite_targets");
        field(rewrites.is_array(), "parts.rewrite_targets");
        for (const auto& entry : rewrites.array_range()) {
            field(entry.is_object() && entry.at("from").is_string(),
                  "parts.rewrite_targets[].from");
            stringArray(entry.at("to"), "parts.rewrite_targets[].to");
        }
        const auto& approximations = parts.at("material_approx");
        field(approximations.is_array(), "parts.material_approx");
        for (const auto& entry : approximations.array_range()) {
            field(entry.is_object() && entry.at("id").is_string() &&
                      entry.at("material_approx").is_bool(),
                  "parts.material_approx[]");
            if (entry.contains("label"))
                field(entry.at("label").is_string(), "parts.material_approx[].label");
        }
        field(unsignedInteger(report.at("welded_interfaces")), "welded_interfaces");
    } else if (report.contains("welded_interfaces")) {
        field(unsignedInteger(report.at("welded_interfaces")), "welded_interfaces");
    }
}
bool supportedVersion(const Json& report) {
    if (!report.is_object() || !report.contains("schema_version") ||
        !report.at("schema_version").is_string())
        return false;
    static const QRegularExpression version(QStringLiteral("\\Aimport-report/1\\.[0-9]+\\z"));
    return version.match(QString::fromStdString(report.at("schema_version").as<std::string>()))
        .hasMatch();
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
    staging_.reset();
    importReportBytes_.clear();
    importReport_ = Json::object();
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
QByteArray ImportController::importReportBytes() const {
    return importReportBytes_;
}
jsoncons::json ImportController::importReport() const {
    return importReport_;
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
    reportCode_.clear();
    grade_.clear();
    QStringList diagnostics;
    bool retainStaging = false;
    // Read each protocol file once, while its original path still exists.
    auto parseReport =
        [&](const QString& path, QByteArray& bytes, Json& parsed, bool optional = false) {
            if (optional && !QFileInfo::exists(path) && !QFileInfo(path).isSymLink())
                return false;
            QFile file(path);
            if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
                diagnostics << tr("导入报告文件读取失败: %1").arg(path);
                return false;
            }
            bytes = file.readAll();
            if (file.error() != QFileDevice::NoError) {
                diagnostics << tr("导入报告文件读取失败: %1 (%2)").arg(path, file.errorString());
                return false;
            }
            try {
                parsed = Json::parse(bytes.toStdString());
            } catch (const std::exception& e) {
                diagnostics
                    << tr("导入报告 JSON 解析失败: %1 (%2)").arg(path, QString::fromUtf8(e.what()));
                retainStaging = true;
                return false;
            }
            if (!supportedVersion(parsed)) {
                diagnostics << tr("导入报告版本不受支持: %1").arg(path);
                return false;
            }
            return true;
        };
    bool validReport = false;
    Json summary;
    if (parseReport(staged_[2], report_, summary)) {
        try {
            field(summary.at("code").is_string() && summary.at("message").is_string() &&
                      summary.at("defects").is_array(),
                  "code/message/defects");
            validateDefects(summary.at("defects"));
            reportCode_ = QString::fromStdString(summary.at("code").as<std::string>());
            if (summary.contains("grade") && summary.at("grade").is_string())
                grade_ = QString::fromStdString(summary.at("grade").as<std::string>());
            validReport = true;
        } catch (const std::exception& e) {
            diagnostics
                << tr("导入报告字段错误: %1 (%2)").arg(staged_[2], QString::fromUtf8(e.what()));
        }
    }
    const auto sidecarPath = QFileInfo(staged_[0]).dir().filePath("import_report.json");
    QByteArray sidecarBytes;
    Json sidecar;
    QElapsedTimer parseTimer;
    parseTimer.start();
    if (parseReport(sidecarPath, sidecarBytes, sidecar, true)) {
        qInfo() << "import_report_parse_ms" << parseTimer.nsecsElapsed() / 1.0e6;
        try {
            validateSidecar(sidecar, request_.kind == ImportKind::Assembly);
            importReportBytes_ = sidecarBytes;
            importReport_ = sidecar;
            QSet<QString> ids;
            for (const auto& defect : sidecar.at("defects").array_range()) {
                const auto id = QString::fromStdString(defect.at("defect_id").as<std::string>());
                if (ids.contains(id))
                    diagnostics << tr("报告内 defect_id 重复: %1 (%2)").arg(id, sidecarPath);
                ids.insert(id);
            }
        } catch (const std::exception& e) {
            diagnostics
                << tr("导入报告字段错误: %1 (%2)").arg(sidecarPath, QString::fromUtf8(e.what()));
        }
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
        error_ = cancelled_
                     ? "Import cancelled"
                     : QString("exit/report mismatch: %1")
                           .arg(validReport ? reportCode_
                                            : QString("missing, corrupt or incompatible report"));
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
    if (!diagnostics.isEmpty()) {
        if (!error_.isEmpty())
            error_ += '\n';
        error_ += diagnostics.join('\n');
    }
    if (!retainStaging)
        staging_.reset();
    busy_ = false;
    if (!cancelled_)
        emit defectsReady();
    emit finished(replaced);
}
} // namespace chipcae::desktop
