#pragma once
#include <QDialog>
namespace chipcae::desktop {
// D09b test-first compile stub; no UI or picker behavior implemented.
class MaterialMapMissingDialog : public QDialog {
public:
    explicit MaterialMapMissingDialog(QWidget* parent = nullptr) : QDialog(parent) {}
    static QString prompt(QWidget* = nullptr) { return {}; }
    QString messageText() const { return {}; }
};
} // namespace chipcae::desktop
