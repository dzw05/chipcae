#pragma once
#include "CaseDocument.hpp"
#include "PreviewWorker.hpp"

#include <QTemporaryDir>
#include <memory>

namespace chipcae::desktop {
// D09b test-first compile surface; behavior intentionally remains unimplemented.
enum class ImportKind { Ecad, Assembly };
enum class ImportOutcome { None, Running, Accepted, NeedsConfirmation, Rejected, Failed };
struct ImportRequest {
    QString input, output, materialMap, polyset, report;
    bool acceptBTier = false;
    ImportKind kind = ImportKind::Ecad;
    QString program = "chipcae-import";
    QStringList prefix;
};
// Reports currently use {"grade":"A|B|C", "defects":[...]}; raw bytes are retained.
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
    // Test-first stubs: preserve the existing implementation until the implementation pass.
    bool start(const ImportRequest& r) {
        return start(r.input, r.output, r.polyset, r.report, r.program, r.prefix);
    }
    ImportOutcome outcome() const { return ImportOutcome::None; }
    QString reportCode() const { return {}; }
    bool restartAccepted() { return false; }
    bool needsMaterialMapReselect() const { return false; }
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
    void userConfirmed(); // Notification only: no continue CLI contract exists yet.
    void finished(bool replaced);

private:
    void complete(int code, QProcess::ExitStatus status);
    CaseDocument& document_;
    std::unique_ptr<QTemporaryDir> staging_;
    PreviewWorker worker_;
    QStringList staged_, destinations_;
    QByteArray stderr_, report_;
    QString error_, grade_;
    bool busy_ = false, cancelled_ = false, awaitingConfirmation_ = false;
    int exitCode_ = -1;
    QProcess::ExitStatus exitStatus_ = QProcess::NormalExit;
};
} // namespace chipcae::desktop
