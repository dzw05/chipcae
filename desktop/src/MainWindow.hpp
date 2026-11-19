#pragma once
#include "CaseDocument.hpp"
#include "EventCodec.hpp"
#include "PreviewOptions.hpp"
#include "Provenance.hpp"
#include "ResultSession.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QProcessEnvironment>
#include <QSet>
#include <QStringList>
class QAction;
class QLabel;
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <optional>
#include <vector>
namespace chipcae::desktop {
class RunMonitor;
class MonitorLogPanel;
class PreviewController;
class PreviewView;
class ImportController;
struct ImportRequest;
enum class ImportKind;
class ResultView;
class LocalJobService;
} // namespace chipcae::desktop
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    MainWindow(const QStringList& args,
               const QProcessEnvironment& env,
               const QString& executableDir = QString(),
               const QString& compileTimeFallback = QString(),
               QWidget* parent = nullptr);
    QString schemaDirectory() const;
    chipcae::desktop::CaseDocument* document() { return &document_; }
    bool openCase(const QString& path);
    bool saveCase(const QString& path) { return document_.save(path); }

    ~MainWindow() override;
    bool importEcadPath(const QString& input,
                        const QString& program = "chipcae-import",
                        const QStringList& prefix = {});
    bool importAssemblyPath(const QString& input,
                            const QString& program = "chipcae-import",
                            const QStringList& prefix = {});
    void cancelImport();
    bool openResultsPath(const QString& manifestPath);
    enum class MeshDigestDialogChoice { Unset, Accept, Cancel };
    void setMeshDigestDialogChoice(MeshDigestDialogChoice choice);
    MeshDigestDialogChoice meshDigestDialogChoice() const;
    chipcae::desktop::MeshSha256Compare lastMeshDigestCompare() const;
    QString lastMeshDigestDetail() const;
    bool lastOpenCancelledForMeshDigest() const;
    bool writePreviewToCase();
    bool writePreviewToCaseEnabled() const;
    bool loadHandwrittenStack(const QString& path = {});
    chipcae::desktop::ImportController* importController() const { return importer_; }
    chipcae::desktop::ResultView* resultView() const { return resultView_; }
    chipcae::desktop::ResultSession* resultSession() const { return session_.get(); }

    void monitorRun(const QString& directory,
                    chipcae::desktop::EventFormat format,
                    chipcae::desktop::LocalJobService* broker = nullptr,
                    const QString& runId = {},
                    const QString& eventFile = "events.caelog");
    // 最近一次 monitorRun 创建的日志面板；尚未监控时返回空指针。
    chipcae::desktop::MonitorLogPanel* monitorLogPanel() const;

    // 使用 previewOptionsPanel()->options()。面板 validationMessage() 非空时不得启动，返回 false。
    bool previewMesh(const QString& program = "chipcae-preview", const QStringList& prefix = {});
    // 使用传入的 options，不读面板、不因面板非法而拒绝（与现有三参数调用一致）。
    bool previewMesh(const QString& program,
                     const QStringList& prefix,
                     const chipcae::desktop::PreviewOptions& options);
    chipcae::desktop::PreviewOptionsPanel* previewOptionsPanel() const;
    chipcae::desktop::PreviewView* previewView() const;
    bool previewDisplayStale() const;
    bool submitCurrentCase();
    void setSubmitProfile(const QJsonObject& p) { profile_ = p; }
    QJsonObject submitProfile() const { return profile_; }
    bool caseIsOpen() const;
    QString sourceBadgeText() const;
    QString sourceBannerText() const;
    QString provenanceWatermark() const;
    QString summaryWithSource(const QString&) const;
    QString monitorStatusText() const { return monitorStatus_; }
    bool captureResults(const QString&);

private:
    QString schemaDirectory_;
    chipcae::desktop::CaseDocument document_;
    chipcae::desktop::ImportController* importer_ = nullptr;
    std::unique_ptr<QTemporaryDir> importOutput_;
    std::optional<chipcae::desktop::ImportKind> currentImportKind_;
    bool startImportRequest(const chipcae::desktop::ImportRequest& request);
    std::unique_ptr<chipcae::desktop::ResultSession> session_;
    chipcae::desktop::ResultView* resultView_ = nullptr;
    QDockWidget* resultDock_ = nullptr;
    std::function<void()> beginImportUi_;
    std::vector<QAction*> importBlockedActions_;
    bool importBusy() const;
    bool workBusy() const;
    void refreshSourceUi();
    QLabel* sourceLabel_ = nullptr;
    QLabel* watermarkLabel_ = nullptr;
    QSet<QString> monitoredRuns_;
    QPointer<chipcae::desktop::MonitorLogPanel> monitorLogPanel_;
    QJsonObject profile_;
    QString casePath_, runId_, runDir_, monitorStatus_;
    bool manualStack_ = false, fallback_ = false;
    int meshNx_ = 4;
    chipcae::desktop::Provenance provenance_;
    chipcae::desktop::LocalJobService* service_ = nullptr;
    chipcae::desktop::PreviewController* preview_ = nullptr;
    chipcae::desktop::PreviewOptionsPanel* previewOptionsPanel_ = nullptr;
    std::unique_ptr<QTemporaryDir> previewOutput_;
    MeshDigestDialogChoice meshDigestDialogChoice_ = MeshDigestDialogChoice::Unset;
    chipcae::desktop::MeshSha256Compare lastMeshDigestCompare_ =
        chipcae::desktop::MeshSha256Compare::PreviewMissing;
    QString lastMeshDigestDetail_;
    bool lastOpenCancelledForMeshDigest_ = false;
    bool previewDigestValid_ = false;
    bool previewDisplayed_ = false;
    bool previewDisplayStale_ = false;
    std::optional<chipcae::desktop::MeshOptions> previewMeshOptions_;
    QAction* writePreviewToCaseAction_ = nullptr;
    void clearPreviewMeshOptions();
};
