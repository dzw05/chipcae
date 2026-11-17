#pragma once
#include "CaseDocument.hpp"

#include <QMainWindow>
class MainWindow : public QMainWindow {
public:
    explicit MainWindow(QWidget* parent = nullptr);
    chipcae::desktop::CaseDocument* document() { return &document_; }
    bool openCase(const QString& path) { return document_.load(path); }
    bool saveCase(const QString& path) { return document_.save(path); }

private:
    chipcae::desktop::CaseDocument document_;
};
