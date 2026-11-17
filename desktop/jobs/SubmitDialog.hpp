#pragma once
#include <QDialog>
#include <QJsonObject>
namespace chipcae::desktop {
// Test-first API stubs, with no profile or persistence behavior.
class SubmitDialog : public QDialog {
public:
    explicit SubmitDialog(QWidget* parent = nullptr) : QDialog(parent) {}
    bool loadProfile(const QString&) { return false; }
    bool saveProfile(const QString&) const { return false; }
    QJsonObject profile() const { return {}; }
    void setProfile(const QJsonObject&) {}
    int meshNx() const { return 0; }
    void setMeshNx(int) {}
};
} // namespace chipcae::desktop
