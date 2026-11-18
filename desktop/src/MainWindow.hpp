#pragma once
#include "CaseDocument.hpp"
#include "EventCodec.hpp"
#include "Provenance.hpp"
#include "ResultSession.hpp"

#include <QJsonObject>
#include <QMainWindow>
#include <QSet>
class QAction;
class QLabel;
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <vector>
namespace chipcae::desktop {
class RunMonitor;
class PreviewController;
class ImportController;
class ResultView;
class LocalJobService;
} // namespace chipcae::desktop
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    chipcae::desktop::CaseDocument* document() { return &document_; }
    bool openCase(const QString& path);
    bool saveCase(const QString& path) { return document_.save(path); }

    ~MainWindow() override;
    bool importEcadPath(const QString& input,
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

    bool previewMesh(const QString& program = "chipcae-preview", const QStringList& prefix = {});
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
    chipcae::desktop::CaseDocument document_;
    chipcae::desktop::ImportController* importer_ = nullptr;
    std::unique_ptr<QTemporaryDir> importOutput_;
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
    QJsonObject profile_;
    QString casePath_, runId_, runDir_, monitorStatus_;
    bool manualStack_ = false, fallback_ = false;
    int meshNx_ = 4;
    chipcae::desktop::Provenance provenance_;
    chipcae::desktop::LocalJobService* service_ = nullptr;
    chipcae::desktop::PreviewController* preview_ = nullptr;
    std::unique_ptr<QTemporaryDir> previewOutput_;
};
