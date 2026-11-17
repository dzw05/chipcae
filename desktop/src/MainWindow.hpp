#pragma once
#include "CaseDocument.hpp"
#include "EventCodec.hpp"
#include "ResultSession.hpp"

#include <QMainWindow>
#include <QTemporaryDir>
#include <functional>
#include <memory>
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
    bool openCase(const QString& path) { return document_.load(path); }
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

private:
    chipcae::desktop::CaseDocument document_;
    chipcae::desktop::ImportController* importer_ = nullptr;
    std::unique_ptr<QTemporaryDir> importOutput_;
    std::unique_ptr<chipcae::desktop::ResultSession> session_;
    chipcae::desktop::ResultView* resultView_ = nullptr;
    QDockWidget* resultDock_ = nullptr;
    std::function<void()> beginImportUi_;
};
