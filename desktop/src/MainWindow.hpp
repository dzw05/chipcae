#pragma once
#include "CaseDocument.hpp"
#include "EventCodec.hpp"

#include <QMainWindow>
namespace chipcae::desktop {
class RunMonitor;
class LocalJobService;
} // namespace chipcae::desktop
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    chipcae::desktop::CaseDocument* document() { return &document_; }
    bool openCase(const QString& path) { return document_.load(path); }
    bool saveCase(const QString& path) { return document_.save(path); }

    void monitorRun(const QString& directory,
                    chipcae::desktop::EventFormat format,
                    chipcae::desktop::LocalJobService* broker = nullptr,
                    const QString& runId = {},
                    const QString& eventFile = "events.caelog");

private:
    chipcae::desktop::CaseDocument document_;
};
