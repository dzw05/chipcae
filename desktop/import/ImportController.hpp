#pragma once
#include "CaseDocument.hpp"
#include "PreviewWorker.hpp"

#include <QTemporaryDir>
#include <functional>
#include <memory>

namespace chipcae::desktop {
enum class ImportKind { Ecad, Assembly };
enum class ImportOutcome { None, Running, Accepted, NeedsConfirmation, Rejected, Failed };
struct ImportRequest {
    QString input, output, materialMap, polyset, report;
    bool acceptBTier = false;
    ImportKind kind = ImportKind::Ecad;
    QString program = "chipcae-import";
    QStringList prefix;
};
class ImportController : public QObject {
    Q_OBJECT
public:
    explicit ImportController(CaseDocument& document, QObject* parent = nullptr);
    ~ImportController() override;
    // prefix supports an interpreter + script; production uses chipcae-import and no prefix.
    bool start(const QString& input,
               const QString& output,
               const QString& polyset = {},
               const QString& report = {},
               const QString& program = "chipcae-import",
               const QStringList& prefix = {});
    bool start(const ImportRequest& request);
    ImportOutcome outcome() const { return outcome_; }
    QString reportCode() const { return reportCode_; }
    bool restartAccepted();
    bool needsMaterialMapReselect() const;
    void cancel();
    bool busy() const { return busy_; }
    int exitCode() const { return exitCode_; }
    QProcess::ExitStatus exitStatus() const { return exitStatus_; }
    QByteArray stderrText() const { return stderr_; }
    QByteArray reportBytes() const { return report_; }
    QString error() const { return error_; }
    QString grade() const { return grade_; }
public slots:
    void confirmDefects();
signals:
    void defectsReady();
    void userConfirmed();
    void finished(bool replaced);

private:
    friend struct ImportControllerTestAccess;
    static QStringList argumentsFor(const ImportRequest& request);
    std::function<bool(const QString&, const QString&)> copyOutput_;
    void complete(int code, QProcess::ExitStatus status);
    CaseDocument& document_;
    std::unique_ptr<QTemporaryDir> staging_;
    PreviewWorker worker_;
    QStringList staged_, destinations_;
    QByteArray stderr_, report_;
    QString error_, grade_, reportCode_;
    ImportRequest request_;
    ImportOutcome outcome_ = ImportOutcome::None;
    bool busy_ = false, cancelled_ = false;
    int exitCode_ = -1;
    QProcess::ExitStatus exitStatus_ = QProcess::NormalExit;
};
} // namespace chipcae::desktop
