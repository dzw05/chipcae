#pragma once
#include <QDialog>
namespace chipcae::desktop {
class MaterialMapMissingDialog : public QDialog {
public:
    explicit MaterialMapMissingDialog(QWidget* parent = nullptr);
    static QString prompt(QWidget* parent = nullptr);
    QString messageText() const;

private:
    QString selected_;
};
} // namespace chipcae::desktop
