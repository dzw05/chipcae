#pragma once
#include <QDialog>
namespace chipcae::desktop {
class MaterialMapMissingDialog : public QDialog {
public:
    explicit MaterialMapMissingDialog(QWidget* parent = nullptr, const QString& startDir = {});
    static QString prompt(QWidget* parent = nullptr, const QString& startDir = {});
    QString messageText() const;

private:
    QString selected_;
};
} // namespace chipcae::desktop
