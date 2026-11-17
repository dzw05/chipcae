#pragma once
#include "CaseDocument.hpp"
#include "EventCodec.hpp"
#include "ResultSession.hpp"

#include <QJsonObject>
#include <QMainWindow>
class QAction;
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <vector>
namespace chipcae::desktop {
class RunMonitor;
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
    bool loadHandwrittenStack(const QString& path = {});
    chipcae::desktop::ImportController* importController() const { return importer_; }
    chipcae::desktop::ResultView* resultView() const { return resultView_; }
    chipcae::desktop::ResultSession* resultSession() const { return session_.get(); }

    void monitorRun(const QString& directory,
                    chipcae::desktop::EventFormat format,
                    chipcae::desktop::LocalJobService* broker = nullptr,
                    const QString& runId = {},
                    const QString& eventFile = "events.caelog");

    // Test-first API stubs: intentionally inert until D14b-2 implementation.
    bool previewMesh(const QString& = "chipcae-preview", const QStringList& = {}) { return false; }
    bool submitCurrentCase() { return false; }
    void setSubmitProfile(const QJsonObject&) {}
    QJsonObject submitProfile() const { return {}; }
    bool caseIsOpen() const { return false; }
    QString sourceBadgeText() const { return {}; }
    QString sourceBannerText() const { return {}; }
    QString provenanceWatermark() const { return {}; }
    QString summaryWithSource(const QString&) const { return {}; }
    QString monitorStatusText() const { return {}; }
    bool captureResults(const QString&) { return false; }

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
};
